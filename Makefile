zabbix_module_lxd: zabbix_module_lxd.c
	gcc -fPIC -shared -o zabbix_module_lxd.so zabbix_module_lxd.c -I../../../include -I../../../src/libs/zbxsysinfo
