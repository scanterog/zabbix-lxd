# zabbix-lxd

A loadable module for Zabbix Agent in order to monitor LXD containers.

## Get the source

When you have Zabbix installed from the official repository:
```
# debian / ubuntu / proxmox
apt install dpkg-dev
apt-get source zabbix-agent
cd zabbix-3.2.x

# redhat / centos
# TODO
```

or directly from svn:

```
mkdir zabbix3.2
cd zabbix3.2
svn co svn://svn.zabbix.com/branches/3.2 .
./bootstrap.sh
```

## Compiling the module

```
./configure --enable-agent
mkdir src/modules/zabbix_module_lxd
cd src/modules/zabbix_module_lxd
wget https://raw.githubusercontent.com/scanterog/zabbix-lxd/master/zabbix_module_lxd.c \
  https://raw.githubusercontent.com/scanterog/zabbix-lxd/master/Makefile
make
```
The output should be a dynamically linked shared object library named `zabbix_module_lxd.so`.

## Install and configure the module

To install the module:

```
mkdir -p /usr/lib/zabbix/modules
cp zabbix_module_lxd.so /usr/lib/zabbix/modules
```

To enable the module, add in `/etc/zabbix/zabbix_agentd.conf`:
```
LoadModule=zabbix_module_lxd.so
```

Finally, restart the zabbix-agent and upload `Zabbix-template-LXD.xml` to your Zabbix server.
