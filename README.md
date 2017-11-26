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

## Disk monitoring

If you run `zabbix_agentd` as a normal zabbix user (the default) you also need to:
- Copy `userparameters_lxd.conf` to `/etc/zabbix/zabbix_agentd.d/`
- Use `visudo` to add `zabbix  ALL=(ALL:ALL) NOPASSWD: /usr/sbin/zabbix_agentd` to /etc/sudoers

If you run `zabbix_agentd` as root:
- Replace all occurences of `lxd.disk` with `lxd.rdisk` in the new disk template

Next:
- remove the old LXD template from zabbix (_without_ clearing)
- import the new `Zabbix-template-LXD-disk.xml` template)
- add the LXD template to your hosts (again)

> If you run an older version of lxc the process id's are probably not stored in `cgroup.procs`, the fallback method in that case is running `lxc-info -n CONTAINERID -p -P /var/lib/lxc`. This is done automatically, but if your lxcpath is different you should probably symlink it. lxc-info uses /tmp for its cache folder with locking information.
