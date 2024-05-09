#!/bin/bash

function install_by_script {
  cd ~/
  wget https://dot.net/v1/dotnet-install.sh
  sudo chmod +x ./dotnet-install.sh
  ./dotnet-install.sh --channel 6.0

  echo 'export DOTNET_ROOT=$HOME/.dotnet' >>~/.bashrc
  echo 'export PATH=$HOME/.dotnet:$HOME/.dotnet/tools:$PATH' >>~/.bashrc
}

function install_by_dnf {
  rpm -Uvh https://packages.microsoft.com/config/centos/8/packages-microsoft-prod.rpm
  dnf install -y dotnet-sdk-6.0 dotnet-runtime-6.0
  mkdir -p /root/.dotnet
  ln -s /usr/bin/dotnet /root/.dotnet/dotnet
}

function install_on_ubuntu {
  apt-get update && \
  apt-get install -y dotnet-sdk-6.0
}


function create_user {
  USERNAME=dev
  USER_UID=1000
  USER_GID=$USER_UID

  # Create the user
  groupadd --gid $USER_GID $USERNAME
  useradd --uid $USER_UID --gid $USER_GID -m $USERNAME

  # [Optional] Add sudo support. Omit if you don't need to install software after connecting.
  apt-get update 
  apt-get install -y sudo

  echo $USERNAME ALL=\(root\) NOPASSWD:ALL >/etc/sudoers.d/$USERNAME
  chmod 0440 /etc/sudoers.d/$USERNAME
}

function install_clangd {
  clangd_dir=/usr/local/lib/clangd

  wget "https://github.com/clangd/clangd/releases/download/17.0.3/clangd-linux-17.0.3.zip" -O /tmp/clangd.zip
  unzip /tmp/clangd.zip -d ${clangd_dir}
  mv ${clangd_dir}/clangd_17.0.3/* ${clangd_dir}
  rm -rf ${clangd_dir}/clangd_17.0.3
  rm -rf /tmp/clangd.zip

  unset clangd_dir
}

function main {
  # need to reload vscode to enable cmake language sever

  # install_by_script
  # install_by_dnf
  install_on_ubuntu

  install_clangd

  mkdir -p /root/.ccache
  echo "max_size = 20.0G" >>/root/.ccache/ccache.conf

  create_user

  echo "unset http_proxy" >>/root/.bashrc
  echo "unset https_proxy" >>/root/.bashrc
  echo 'export PATH=/usr/local/lib/clangd/bin/:$PATH' >>/root/.bashrc

  # enable coredump
  echo "* soft core unlimited" >>/etc/security/limits.conf
  sysctl -w kernel.core_pattern="/coredumps/core-%e-%s-%u-%g-%p-%t"
}

main $@
