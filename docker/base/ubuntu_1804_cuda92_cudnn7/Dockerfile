FROM nvidia/cuda:9.2-cudnn7-devel-ubuntu18.04

RUN apt-get update --quiet && \
    apt-get install --no-install-recommends --no-install-suggests --yes  \
    software-properties-common apt-utils wget build-essential cython emacs python-dev python-pip python3-dev python3-pip libhdf5-serial-dev cmake git-core libboost-all-dev libfftw3-dev h5utils jq hdf\
5-tools libatlas-base-dev liblapack-dev libxml2-dev libfreetype6-dev pkg-config libxslt-dev libarmadillo-dev libace-dev gcc-multilib libgtest-dev python3-dev liblapack-dev liblapacke-dev libplplot-dev libdcmtk-dev sup\
ervisor cmake-curses-gui neofetch supervisor net-tools cpio gpg-agent

RUN apt-get install --no-install-recommends --no-install-suggests --yes libopenblas-base libopenblas-dev

#Python stuff
RUN apt-get update && apt-get install -y libgtk2.0-dev

RUN pip3 install --upgrade pip
RUN pip3 install -U pip setuptools

# tensorflow does not work with cuda92
RUN pip3 install --upgrade tensorflow 
# use pytorch 0.4.0
RUN pip3 install http://download.pytorch.org/whl/cpu/torch-0.4.0-cp36-cp36m-linux_x86_64.whl  
RUN pip3 install torchvision
RUN pip3 install tensorboardx visdom

RUN apt-get install --no-install-recommends --no-install-suggests --yes python3-psutil python3-pyxb python3-lxml python3-numpy
RUN apt-get install --no-install-recommends --no-install-suggests --yes python3-pil
RUN apt-get install --no-install-recommends --no-install-suggests --yes python3-scipy
RUN apt-get install --no-install-recommends --no-install-suggests --yes python3-configargparse
RUN pip3 install Cython tk-tools matplotlib scikit-image opencv_python pydicom scikit-learn
RUN DEBIAN_FRONTEND=noninteractive apt-get install --no-install-recommends --no-install-suggests --yes python3-tk
RUN pip3 uninstall h5py
RUN apt-get install -y python3-h5py

# since cmake has problems to find python3 and boost-python3
# RUN ln -s /usr/lib/x86_64-linux-gnu/libboost_python-py36.so /usr/lib/x86_64-linux-gnu/libboost_python3.so

# fix the  qhull reentrant problem
# RUN pip uninstall -y scipy

RUN mkdir /opt/code

#ZFP
RUN cd /opt && \
    git clone https://github.com/hansenms/ZFP.git && \
    cd ZFP && \
    mkdir lib && \
    make && \
    make shared && \
    make -j $(nproc) install

#BART
RUN cd /opt && \
    wget https://github.com/mrirecon/bart/archive/v0.4.03.tar.gz && \
    tar -xzf v0.4.03.tar.gz && \
    cd bart-0.4.03 && \
    make -j $(nproc) && \
    ln -s /opt/bart-v0.4.03/bart /usr/local/bin/bart 

# ceres
RUN apt-get install --yes libgoogle-glog-dev libeigen3-dev libsuitesparse-dev
RUN cd /opt && \
    wget http://ceres-solver.org/ceres-solver-1.14.0.tar.gz && \
    tar zxf ceres-solver-1.14.0.tar.gz && \
    mkdir ceres-bin && \
    cd ceres-bin && \
    cmake ../ceres-solver-1.14.0 && \
    make -j20 && \
    make install

#Set more environment variables in preparation for Gadgetron installation
ENV GADGETRON_HOME=/usr/local \
    ISMRMRD_HOME=/usr/local

ENV PATH=$PATH:/usr/local/cuda-9.2/bin;$GADGETRON_HOME/bin:$ISMRMRD_HOME/bin \
    LD_LIBRARY_PATH=$LD_LIBRARY_PATH:/usr/local/cuda-9.2/lib64:$ISMRMRD_HOME/lib:$GADGETRON_HOME/lib

ENV LIBRARY_PATH /usr/local/cuda/lib64/stubs:${LIBRARY_PATH}

# Clean up packages.
#RUN  apt-get clean && \
#   rm -rf /var/lib/apt/lists/*
