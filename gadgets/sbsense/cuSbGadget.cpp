#include "cuNDArray_operators.h"
#include "cuNDArray_elemwise.h"
#include "cuNDArray_blas.h"
#include "cuNDArray_utils.h"
#include "vector_td_utilities.h"
#include "b1_map.h"
#include "cuSbGadget.h"
#include "Gadgetron.h"
#include "GadgetMRIHeaders.h"
#include "GadgetIsmrmrdReadWrite.h"
#include "hoNDArray_fileio.h"

namespace Gadgetron{

  cuSbGadget::cuSbGadget()
  : slice_no_(0)
  , profiles_per_frame_(32)
  , shared_profiles_(0)
  , channels_(0)
  , samples_per_profile_(0)
  , device_number_(0)
  , number_of_cg_iterations_(10)
  , number_of_sb_iterations_(10)
  , cg_limit_(1e-6)
  , oversampling_(1.25)
  , kernel_width_(5.5)
  , mu_(1.0)
  , lambda_(2.0)
  , alpha_(0.5)
  , current_profile_offset_(0)
  , allocated_samples_(0)
  , data_host_ptr_(0x0)
  , is_configured_(false)
  , dcw_computed_(false)
  , image_series_(0)
  , image_counter_(0)
  , mutex_("cuSbMutex")
  , buffer_(ACE_Message_Queue_Base::DEFAULT_HWM * 10, ACE_Message_Queue_Base::DEFAULT_LWM * 10)
  {
    matrix_size_ = uintd2(0,0);
    matrix_size_os_ = uintd2(0,0);
    memset(position_, 0, 3*sizeof(float));
    memset(read_dir_, 0, 3*sizeof(float));
    memset(phase_dir_, 0, 3*sizeof(float));
    memset(slice_dir_, 0, 3*sizeof(float));
    pass_on_undesired_data_ = true; // We will make one of these for each slice and so data should be passed on.
  }

  cuSbGadget::~cuSbGadget() {}

  int cuSbGadget::process_config( ACE_Message_Block* mb )
  {
    GADGET_DEBUG1("\ncuSbGadget::process_config\n");

    slice_no_ = get_int_value(std::string("sliceno").c_str());

    device_number_ = get_int_value(std::string("deviceno").c_str());

    int number_of_devices = 0;
    if (cudaGetDeviceCount(&number_of_devices)!= cudaSuccess) {
      GADGET_DEBUG1( "Error: unable to query number of CUDA devices.\n" );
      return GADGET_FAIL;
    }

    if (number_of_devices == 0) {
      GADGET_DEBUG1( "Error: No available CUDA devices.\n" );
      return GADGET_FAIL;
    }

    if (device_number_ >= number_of_devices) {
      GADGET_DEBUG2("Adjusting device number from %d to %d\n", device_number_,  (device_number_%number_of_devices));
      device_number_ = (device_number_%number_of_devices);
    }

    if (cudaSetDevice(device_number_)!= cudaSuccess) {
      GADGET_DEBUG1( "Error: unable to set CUDA device.\n" );
      return GADGET_FAIL;
    }

    profiles_per_frame_ = get_int_value(std::string("profiles_per_frame").c_str());
    shared_profiles_ = get_int_value(std::string("shared_profiles").c_str());
    number_of_cg_iterations_ = get_int_value(std::string("number_of_cg_iterations").c_str());
    number_of_sb_iterations_ = get_int_value(std::string("number_of_sb_iterations").c_str());
    cg_limit_ = get_double_value(std::string("cg_limit").c_str());
    oversampling_ = get_double_value(std::string("oversampling").c_str());
    kernel_width_ = get_double_value(std::string("kernel_width").c_str());
    mu_ = get_double_value(std::string("mu").c_str());
    lambda_ = get_double_value(std::string("lambda").c_str());
    alpha_ = get_double_value(std::string("alpha").c_str());
    pass_on_undesired_data_ = get_bool_value(std::string("pass_on_undesired_data").c_str());
    image_series_ = this->get_int_value("image_series");

    if( shared_profiles_ > (profiles_per_frame_-1) ){
      GADGET_DEBUG1("\nWARNING: cuSbGadget::process_config: shared_profiles exceeds profiles_per_frame-1.\n");
      shared_profiles_ = profiles_per_frame_-1;
    }

    boost::shared_ptr<ISMRMRD::ismrmrdHeader> cfg = parseIsmrmrdXMLHeader(std::string(mb->rd_ptr()));

    std::vector<long> dims;
    ISMRMRD::ismrmrdHeader::encoding_sequence e_seq = cfg->encoding();
    if (e_seq.size() != 1) {
      GADGET_DEBUG2("Number of encoding spaces: %d\n", e_seq.size());
      GADGET_DEBUG1("This Gadget only supports one encoding space\n");
      return GADGET_FAIL;
    }

    ISMRMRD::encodingSpaceType e_space = (*e_seq.begin()).encodedSpace();
    ISMRMRD::encodingSpaceType r_space = (*e_seq.begin()).reconSpace();
    ISMRMRD::encodingLimitsType e_limits = (*e_seq.begin()).encodingLimits();

    if (!is_configured_) {

      cudaDeviceProp deviceProp;
      if( cudaGetDeviceProperties( &deviceProp, device_number_ ) != cudaSuccess) {
	GADGET_DEBUG1( "\nError: unable to query device properties.\n" );
	return GADGET_FAIL;
      }

      unsigned int warp_size = deviceProp.warpSize;

      samples_per_profile_ = e_space.matrixSize().x();
      channels_ = cfg->acquisitionSystemInformation().present() && cfg->acquisitionSystemInformation().get().receiverChannels().present() ?
	cfg->acquisitionSystemInformation().get().receiverChannels().get() : 1;

      matrix_size_ = uintd2(e_space.matrixSize().x(), e_space.matrixSize().y());

      GADGET_DEBUG2("\nMatrix size  : [%d,%d] \n", matrix_size_.vec[0], matrix_size_.vec[1]);

      matrix_size_os_ =
	uintd2(static_cast<unsigned int>(std::ceil((matrix_size_.vec[0]*oversampling_)/warp_size)*warp_size),
	       static_cast<unsigned int>(std::ceil((matrix_size_.vec[1]*oversampling_)/warp_size)*warp_size));

      GADGET_DEBUG2("\nMatrix size OS: [%d,%d] \n", matrix_size_os_.vec[0], matrix_size_os_.vec[1]);

      GADGET_DEBUG2("Using device number %d for slice %d\n", device_number_, slice_no_);

      // Allocate encoding operator for non-Cartesian Sense
      std::vector<unsigned int> image_dims = to_std_vector(matrix_size_);

      E_ = boost::shared_ptr< cuNonCartesianSenseOperator<float,2> >( new cuNonCartesianSenseOperator<float,2>() );
      E_->set_domain_dimensions(&image_dims);

      // Allocate preconditioner
      //D_ = boost::shared_ptr< cuCgPreconditioner<float_complext> >( new cuCgPreconditioner<float_complext>() );

      // Define regularization operators 
      // We need "a pair" for PICCS
      //
      
      Rx1_ = boost::shared_ptr< cuPartialDerivativeOperator<float_complext,2> >
	( new cuPartialDerivativeOperator<float_complext,2>(0) );
      Rx1_->set_weight( (1.0-alpha_)*lambda_ );
      Rx1_->set_domain_dimensions(&image_dims);
      Rx1_->set_codomain_dimensions(&image_dims);

      Ry1_ = boost::shared_ptr< cuPartialDerivativeOperator<float_complext,2> >
	( new cuPartialDerivativeOperator<float_complext,2>(1) );
      Ry1_->set_weight( (1.0-alpha_)*lambda_ );
      Ry1_->set_domain_dimensions(&image_dims);
      Ry1_->set_codomain_dimensions(&image_dims);

      /*Rz1_ = boost::shared_ptr< cuPartialDerivativeOperator<float_complext,3> >
	( new cuPartialDerivativeOperator<float_complext,3>(2) );
      Rz1_->set_weight( (1.0-alpha_)*lambda_ );
      Rz1_->set_domain_dimensions(&image_dims);
      Rz1_->set_codomain_dimensions(&image_dims);*/

      Rx2_ = boost::shared_ptr< cuPartialDerivativeOperator<float_complext,2> >
	( new cuPartialDerivativeOperator<float_complext,2>(0) );
      Rx2_->set_weight( alpha_*lambda_ );
      Rx2_->set_domain_dimensions(&image_dims);
      Rx2_->set_codomain_dimensions(&image_dims);

      Ry2_ = boost::shared_ptr< cuPartialDerivativeOperator<float_complext,2> >
	( new cuPartialDerivativeOperator<float_complext,2>(1) );
      Ry2_->set_weight( alpha_*lambda_ );
      Ry2_->set_domain_dimensions(&image_dims);
      Ry2_->set_codomain_dimensions(&image_dims);

      /*Rz2_ = boost::shared_ptr< cuPartialDerivativeOperator<float_complext,3> >
	( new cuPartialDerivativeOperator<float_complext,3>(2) );
      Rz2_->set_weight( alpha_*lambda_ );
      Rz2_->set_domain_dimensions(&image_dims);
      Rz2_->set_codomain_dimensions(&image_dims);*/

      // Setup split-Bregman solver
      sb_.set_encoding_operator( E_ );
      
      // Add "TV" regularization
      if( alpha_<1.0 ){
	sb_.add_regularization_group_operator( Rx1_ ); 
	sb_.add_regularization_group_operator( Ry1_ ); 
	//sb_.add_regularization_group_operator( Rz1_ ); 
	sb_.add_group();
      }
      
      // Add "PICCS" regularization
      /*if( alpha_ > 0.0 ){
	sb_.add_regularization_group_operator( Rx2_ ); 
	sb_.add_regularization_group_operator( Ry2_ ); 
	//sb_.add_regularization_group_operator( Rz2_ ); 
	sb_.add_group(reg_image.get());  <--- !!!!
	}*/
      
      sb_.set_max_outer_iterations(number_of_sb_iterations_);
      sb_.set_max_inner_iterations(1);
      sb_.set_output_mode( cuSbcCgSolver<float_complext>::OUTPUT_WARNINGS );
      
      sb_.get_inner_solver()->set_max_iterations( number_of_cg_iterations_ );
      sb_.get_inner_solver()->set_tc_tolerance( cg_limit_ );
      sb_.get_inner_solver()->set_output_mode( cuCgSolver<float_complext>::OUTPUT_WARNINGS );
      //sb_.get_inner_solver()->set_preconditioner( D_ );  <--- !!!

      if( configure_channels() == GADGET_FAIL )
	return GADGET_FAIL;

      is_configured_ = true;
    }

    return GADGET_OK;
  }


  int cuSbGadget::configure_channels()
  {
    // We do not have a csm yet, so initialize a dummy one to purely ones

    std::vector<unsigned int> csm_dims = to_std_vector(matrix_size_); csm_dims.push_back( channels_ );
    boost::shared_ptr< cuNDArray<float_complext> > csm( new cuNDArray<float_complext> );

    try { csm->create( &csm_dims ); }
    catch ( cuda_error &err){
      GADGET_DEBUG_EXCEPTION(err, "Failed to create csm array \n" );
      return GADGET_FAIL;
    }

    try { fill(csm.get(), float_complext(1)); }
    catch ( cuda_error &err){
      GADGET_DEBUG_EXCEPTION(err, "Failed to fill csm array \n" );
      return GADGET_FAIL;
    }

    // Setup matrix operator
    E_->set_csm(csm);

    try {E_->setup( matrix_size_, matrix_size_os_, kernel_width_ );}
    catch (cuda_error &err){
      GADGET_DEBUG_EXCEPTION(err, "\nError: unable to setup encoding operator.\n" );
      return GADGET_FAIL;
    }

    // Allocate rhs buffer
    rhs_buffer_ = boost::shared_ptr< cuSenseRHSBuffer<float,2> >( new cuSenseRHSBuffer<float,2>() );
    rhs_buffer_->set_num_coils( channels_ );
    rhs_buffer_->set_sense_operator( E_ );
    return GADGET_OK;
  }

  int cuSbGadget::process(GadgetContainerMessage<ISMRMRD::AcquisitionHeader>* m1, GadgetContainerMessage< hoNDArray< std::complex<float> > >* m2)
  {

    if (!is_configured_) {
      GADGET_DEBUG1("\nData received before configuration complete\n");
      return GADGET_FAIL;
    }

    //Is this data for me?
    if (m1->getObjectPtr()->idx.slice != slice_no_) {

      //This data is not for me
      if (pass_on_undesired_data_) {
	this->next()->putq(m1);
      } else {
	GADGET_DEBUG2("Dropping slice: %d\n", m1->getObjectPtr()->idx.slice);
	m1->release();
      }
      return GADGET_OK;
    }

    mutex_.acquire();

    // Check if some upstream gadget has modified the number of channels or samples per profile
    // since the global configuration is no longer valid then...
    //

    if( m1->getObjectPtr()->number_of_samples != samples_per_profile_ ) {
      GADGET_DEBUG2("Adjusting #samples per profile from %d to %d\n", samples_per_profile_,  m1->getObjectPtr()->number_of_samples );
      samples_per_profile_ = m1->getObjectPtr()->number_of_samples;
      allocated_samples_ = 0; // the samples buffers are freed and re-allocated in 'upload_samples()'
    }

    if( m1->getObjectPtr()->active_channels != channels_ ) {
      GADGET_DEBUG2("Adjusting #channels from %d to %d\n", channels_,  m1->getObjectPtr()->active_channels );
      channels_ = m1->getObjectPtr()->active_channels;
      allocated_samples_ = 0; // the samples buffers are freed and re-allocated in 'upload_samples()'
      if( configure_channels() == GADGET_FAIL ) // Update buffers dependant on #channels
	return GADGET_FAIL;
    }

    // Check to see of the imaging plane has changed
    if ( !read_dir_equal(m1->getObjectPtr()->read_dir) || 
	 !phase_dir_equal(m1->getObjectPtr()->phase_dir) ||
	 !slice_dir_equal(m1->getObjectPtr()->slice_dir) ||
	 !position_equal(m1->getObjectPtr()->position)) {
      rhs_buffer_->clear();
      memcpy(position_,m1->getObjectPtr()->position,3*sizeof(float));
      memcpy(read_dir_,m1->getObjectPtr()->read_dir,3*sizeof(float));
      memcpy(phase_dir_,m1->getObjectPtr()->phase_dir,3*sizeof(float));
      memcpy(slice_dir_,m1->getObjectPtr()->slice_dir,3*sizeof(float));
    }

    buffer_.enqueue_tail(m1);

    if ((int)buffer_.message_count() >= profiles_per_frame_) {

      boost::shared_ptr< cuNDArray<floatd2> > traj = calculate_trajectory();

      if ( traj.get() == 0x0 ) {
	GADGET_DEBUG1("\nFailed to calculate trajectory\n");
	return GADGET_FAIL;
      }

      boost::shared_ptr< cuNDArray<float> > dcw;
      if( !dcw_computed_){
	dcw = calculate_density_compensation();
	if( dcw.get() == 0x0 ) {
	  GADGET_DEBUG1("\nFailed to calculate density compensation\n");
	  return GADGET_FAIL;
	}
	E_->set_dcw(dcw);
	dcw_computed_ = false;
      }

      boost::shared_ptr< cuNDArray<float_complext> > device_samples = upload_samples();
      if( device_samples == 0x0 ) {
	GADGET_DEBUG1("\nFailed to upload samples to the GPU\n");
	return GADGET_FAIL;
      }

      try{ E_->preprocess(traj.get());}
      catch (runtime_error& err){
	GADGET_DEBUG_EXCEPTION(err, "\nError during cgOperatorNonCartesianSense::preprocess()\n");
	return GADGET_FAIL;
      }

      rhs_buffer_->add_frame_data( device_samples.get(), traj.get() );

      boost::shared_ptr< cuNDArray<float_complext> > csm_data = rhs_buffer_->get_acc_coil_images();
      if( !csm_data.get() ){
	GADGET_DEBUG1("\nError during accumulation buffer computation\n");
	return GADGET_FAIL;
      }

      // Estimate CSM
      boost::shared_ptr< cuNDArray<float_complext> > csm = estimate_b1_map<float,2>( csm_data.get() );
      E_->set_csm(csm);

      boost::shared_ptr< std::vector<unsigned int> > reg_dims = csm_data->get_dimensions();
      reg_dims->pop_back();

      cuNDArray<float_complext> reg_image;
      try {reg_image.create(reg_dims.get()); }
      catch (cuda_error& err){
	GADGET_DEBUG_EXCEPTION(err,"\nError allocating regularization image on device\n");
	return GADGET_FAIL;
      }

      try{ E_->mult_csm_conj_sum( csm_data.get(), &reg_image );}
      catch (runtime_error& err){
	GADGET_DEBUG_EXCEPTION(err,"\nError combining coils to regularization image\n");
	return GADGET_FAIL;
      }

      //R_->compute(&reg_image);
      /*
      // Define preconditioning weights
      boost::shared_ptr< cuNDArray<float> > _precon_weights = sum(abs_square(csm.get()).get(), 2);
      boost::shared_ptr<cuNDArray<float> > R_diag = R_->get();
      *R_diag *= float(kappa_);
      *_precon_weights += *R_diag;
      R_diag.reset();
      reciprocal_sqrt_inplace(_precon_weights.get());
      boost::shared_ptr< cuNDArray<float_complext> > precon_weights = real_to_complex<float_complext>( _precon_weights.get() );
      _precon_weights.reset();
      D_->set_weights( precon_weights );
      */

      // Invoke solver
      //
      
      boost::shared_ptr< cuNDArray<float_complext> > sbresult;
      
      try{ 
	sbresult = sb_.solve(device_samples.get()); }
      catch (runtime_error& err){
	GADGET_DEBUG_EXCEPTION(err,"\nError during solve()\n");
	return GADGET_FAIL;
      }
      
      if (!sbresult.get()) {
	GADGET_DEBUG1("\nSplit Bregman solver failed\n");
	return GADGET_FAIL;
      }

      //Now pass the reconstructed image on
      GadgetContainerMessage<ISMRMRD::ImageHeader>* cm1 =
	new GadgetContainerMessage<ISMRMRD::ImageHeader>();

      GadgetContainerMessage< hoNDArray< std::complex<float> > >* cm2 =
	new GadgetContainerMessage< hoNDArray< std::complex<float> > >();

      cm1->cont(cm2);

      std::vector<unsigned int> img_dims(2);
      img_dims[0] = matrix_size_.vec[0];
      img_dims[1] = matrix_size_.vec[1];

      try{cm2->getObjectPtr()->create(&img_dims);}
      catch (runtime_error &err){
	GADGET_DEBUG_EXCEPTION(err,"\nUnable to allocate host image array");
	cm1->release();
	return GADGET_FAIL;
      }

      size_t data_length = prod(matrix_size_);

      cudaMemcpy(cm2->getObjectPtr()->get_data_ptr(),
		 sbresult->get_data_ptr(),
		 data_length*sizeof(std::complex<float>),
		 cudaMemcpyDeviceToHost);

      cudaError_t err = cudaGetLastError();
      if( err != cudaSuccess ){
	GADGET_DEBUG2("\nUnable to copy result from device to host: %s", cudaGetErrorString(err));
	cm1->release();
	return GADGET_FAIL;
      }

      cm1->getObjectPtr()->matrix_size[0] = img_dims[0];
      cm1->getObjectPtr()->matrix_size[1] = img_dims[1];
      cm1->getObjectPtr()->matrix_size[2] = 1;
      cm1->getObjectPtr()->channels       = 1;
      cm1->getObjectPtr()->slice          = m1->getObjectPtr()->idx.slice;
      cm1->getObjectPtr()->acquisition_time_stamp     = m1->getObjectPtr()->acquisition_time_stamp;

      memcpy(cm1->getObjectPtr()->position,m1->getObjectPtr()->position, sizeof(float)*3);
      memcpy(cm1->getObjectPtr()->read_dir,m1->getObjectPtr()->read_dir, sizeof(float)*3);
      memcpy(cm1->getObjectPtr()->phase_dir,m1->getObjectPtr()->phase_dir, sizeof(float)*3);
      memcpy(cm1->getObjectPtr()->slice_dir,m1->getObjectPtr()->slice_dir, sizeof(float)*3);
      memcpy(cm1->getObjectPtr()->patient_table_position, m1->getObjectPtr()->patient_table_position, sizeof(float)*3);

      cm1->getObjectPtr()->image_index = ++image_counter_;
      cm1->getObjectPtr()->image_series_index = image_series_;

      if (this->next()->putq(cm1) < 0) {
	GADGET_DEBUG1("\nFailed to result image on to Q\n");
	cm1->release();
	return GADGET_FAIL;
      }

      //Dequeue the message we don't need anymore
      ACE_Message_Block* mb_tmp;
      for (int i = 0; i < (profiles_per_frame_-shared_profiles_); i++) {
	buffer_.dequeue_head(mb_tmp);
	mb_tmp->release();
	current_profile_offset_++;
      }
    }

    mutex_.release();

    return GADGET_OK;
  }


  int cuSbGadget::copy_samples_for_profile(float* host_base_ptr,
					   std::complex<float>* data_base_ptr,
					   int profile_no,
					   int channel_no)
  {

    memcpy(host_base_ptr +
	   (channel_no*allocated_samples_ + profile_no*samples_per_profile_) * 2,
	   data_base_ptr + channel_no*samples_per_profile_,
	   sizeof(float)*samples_per_profile_*2);

    return GADGET_OK;
  }

  boost::shared_ptr< cuNDArray<float_complext> >  cuSbGadget::upload_samples()
  {

    int samples_needed =
      samples_per_profile_*
      profiles_per_frame_;

    if (samples_needed != allocated_samples_) {

      if( data_host_ptr_ ){
	delete[] data_host_ptr_;
	data_host_ptr_ = 0x0;
	allocated_samples_ = 0;
      }

      try {
	data_host_ptr_ = new float[channels_*samples_needed*2];
      } catch (...) {
	GADGET_DEBUG1("\nFailed to allocate host memory for samples\n");
	return boost::shared_ptr< cuNDArray<float_complext> >();
      }

      allocated_samples_ = samples_needed;

    }

    ACE_Message_Queue_Reverse_Iterator<ACE_MT_SYNCH> it(buffer_);
    int profiles_copied = 0;
    GadgetContainerMessage<ISMRMRD::AcquisitionHeader>* m1;
    GadgetContainerMessage< hoNDArray< std::complex<float> > >* m2;
    ACE_Message_Block* mb;

    while (profiles_copied < profiles_per_frame_) {
      it.next(mb);

      m1 = dynamic_cast< GadgetContainerMessage< ISMRMRD::AcquisitionHeader >* >(mb);
      if (!m1) {
	GADGET_DEBUG1("\nFailed to dynamic cast message\n");
	return boost::shared_ptr< cuNDArray<float_complext> >();
      }

      m2 = dynamic_cast< GadgetContainerMessage< hoNDArray< std::complex<float> > >* > (m1->cont());

      if (!m2) {
	GADGET_DEBUG1("\nFailed to dynamic cast message\n");
	return boost::shared_ptr< cuNDArray<float_complext> >();
      }

      std::complex<float> *d = m2->getObjectPtr()->get_data_ptr();
      int current_profile = profiles_per_frame_-profiles_copied-1;

      for (int i = 0; i < channels_; i++) {
	copy_samples_for_profile( data_host_ptr_, d, current_profile, i );
      }

      it.advance();
      profiles_copied++;
    }

    std::vector<unsigned int> dims; dims.push_back(samples_needed); dims.push_back(channels_);
    hoNDArray<float_complext> tmp;
    try{ tmp.create( &dims, (float_complext*)data_host_ptr_, false );}
    catch (runtime_error &err){
      GADGET_DEBUG1("\nFailed to create temporary host data array\n");
      return boost::shared_ptr< cuNDArray<float_complext> >();
    }

    boost::shared_ptr< cuNDArray<float_complext> > device_samples ( new cuNDArray<float_complext>(&tmp) );

    cudaError_t err = cudaGetLastError();
    if( err != cudaSuccess ){
      GADGET_DEBUG2("\nUnable to upload samples to GPU memory: %s", cudaGetErrorString(err));
      return boost::shared_ptr< cuNDArray<float_complext> >();
    }

    return device_samples;
  }

  int cuSbGadget::parameter_changed(std::string name, std::string new_value, std::string old_value)
  {
    mutex_.acquire();
    GADGET_DEBUG2("cuSbGadget, changing parameter %s: %s -> %s\n", name.c_str(), old_value.c_str(), new_value.c_str());

    if (name.compare("profiles_per_frame") == 0) {
      profiles_per_frame_ = get_int_value(std::string("profiles_per_frame").c_str());
    }
    mutex_.release();

    return GADGET_OK;
  }
}