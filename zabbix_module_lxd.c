/*
** Zabbix module for LXD container monitoring
** Script based on Jan Garaj from www.monitoringartist.com
** Author: Samuel Cantero <samuel.cantero@sourcefabric.org>
**
** This program is free software; you can redistribute it and/or modify
** it under the terms of the GNU General Public License as published by
** the Free Software Foundation; either version 2 of the License, or
** (at your option) any later version.
**
** This program is distributed in the hope that it will be useful,
** but WITHOUT ANY WARRANTY; without even the implied warranty of
** MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
** GNU General Public License for more details.
**
** You should have received a copy of the GNU General Public License
** along with this program; if not, write to the Free Software
** Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
**/

#include "common.h"
#include "log.h"
#include "comms.h"
#include "module.h"
#include "sysinc.h"
#include "zbxjson.h"
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/statvfs.h>
#include <sys/un.h>
#include <grp.h>

// request parameters
#include "common/common.h"
#include "sysinfo.c"

#ifndef ZBX_MODULE_API_VERSION
#       define ZBX_MODULE_API_VERSION   ZBX_MODULE_API_VERSION_ONE
#endif

struct inspect_result
{
   char  *value;
   int   return_code;
};

char    *m_version = "v0.1";
char    *stat_dir = NULL, *driver, *cpu_cgroup = NULL, *hostname = 0;
static int item_timeout = 1, buffer_size = 1024, cid_length = 66, socket_api;
int     zbx_module_lxd_discovery(AGENT_REQUEST *request, AGENT_RESULT *result);
int     zbx_module_lxd_up(AGENT_REQUEST *request, AGENT_RESULT *result);
int     zbx_module_lxd_mem(AGENT_REQUEST *request, AGENT_RESULT *result);
int     zbx_module_lxd_cpu(AGENT_REQUEST *request, AGENT_RESULT *result);
int     zbx_module_lxd_dev(AGENT_REQUEST *request, AGENT_RESULT *result);
int     zbx_module_lxd_disk(AGENT_REQUEST *request, AGENT_RESULT *result);

static ZBX_METRIC keys[] =
/*      KEY                     FLAG            FUNCTION                TEST PARAMETERS */
{
        {"lxd.discovery", CF_HAVEPARAMS, zbx_module_lxd_discovery,    "<parameter 1>, <parameter 2>, <parameter 3>"},
        {"lxd.up",   CF_HAVEPARAMS,  zbx_module_lxd_up,   "container name"},
        {"lxd.mem",  CF_HAVEPARAMS,  zbx_module_lxd_mem,  "container name, memory metric name"},
        {"lxd.cpu",  CF_HAVEPARAMS,  zbx_module_lxd_cpu,  "container name, cpu metric name"},
        {"lxd.dev",  CF_HAVEPARAMS,  zbx_module_lxd_dev,  "container name, blkio file, blkio metric name"},
        {"lxd.rdisk", CF_HAVEPARAMS,  zbx_module_lxd_disk,  "container name, disk metric name"},
        {NULL}
};

/******************************************************************************
 *                                                                            *
 * Function: zbx_module_api_version                                           *
 *                                                                            *
 * Purpose: returns version number of the module interface                    *
 *                                                                            *
 * Return value: ZBX_MODULE_API_VERSION_ONE - the only version supported by   *
 *               Zabbix currently                                             *
 *                                                                            *
 ******************************************************************************/
int     zbx_module_api_version()
{
        zabbix_log(LOG_LEVEL_DEBUG, "In zbx_module_api_version()");
        return ZBX_MODULE_API_VERSION_ONE;
}

/******************************************************************************
 *                                                                            *
 * Function: zbx_module_item_timeout                                          *
 *                                                                            *
 * Purpose: set timeout value for processing of items                         *
 *                                                                            *
 * Parameters: timeout - timeout in seconds, 0 - no timeout set               *
 *                                                                            *
 ******************************************************************************/
void    zbx_module_item_timeout(int timeout)
{
        zabbix_log(LOG_LEVEL_DEBUG, "In zbx_module_item_timeout()");
        item_timeout = timeout;
}

/******************************************************************************
 *                                                                            *
 * Function: zbx_module_item_list                                             *
 *                                                                            *
 * Purpose: returns list of item keys supported by the module                 *
 *                                                                            *
 * Return value: list of item keys                                            *
 *                                                                            *
 ******************************************************************************/
ZBX_METRIC      *zbx_module_item_list()
{
        zabbix_log(LOG_LEVEL_DEBUG, "In zbx_module_item_list()");
        return keys;
}

/******************************************************************************
 *                                                                            *
 * Function: zbx_lxd_dir_detect                                               *
 *                                                                            *
 * Purpose: it should find metric folder - it depends on exec environment     *
 *                                                                            *
 * Return value: SYSINFO_RET_FAIL - stat folder was not found                 *
 *               SYSINFO_RET_OK - stat folder was found                       *
 *                                                                            *
 ******************************************************************************/
int     zbx_lxd_dir_detect()
{
        zabbix_log(LOG_LEVEL_DEBUG, "In zbx_lxd_dir_detect()");

        char *drivers[] = {
            "lxc/",           // LXC
            ""
        }, **tdriver;
        char path[512];
        char *temp1, *temp2;
        FILE *fp;
        DIR  *dir;

        if ((fp = fopen("/proc/mounts", "r")) == NULL)
        {
            zabbix_log(LOG_LEVEL_WARNING, "Cannot open /proc/mounts: %s", zbx_strerror(errno));
            return SYSINFO_RET_FAIL;
        }

        while (fgets(path, 512, fp) != NULL)
        {
            if ((strstr(path, "cpuset cgroup")) != NULL)
            {
                temp1 = string_replace(path, "cgroup ", "");
                temp2 = string_replace(temp1, strstr(temp1, " "), "");
                free(temp1);
                if (stat_dir != NULL) free(stat_dir);
                stat_dir = string_replace(temp2, "cpuset", "");
                free(temp2);
                zabbix_log(LOG_LEVEL_DEBUG, "Detected LXD stat directory: %s", stat_dir);


                pclose(fp);

                char *cgroup = "cpuset/";
                tdriver = drivers;
                size_t  ddir_size;
                char    *ddir;
                while (*tdriver != "")
                {
                    ddir_size = strlen(cgroup) + strlen(stat_dir) + strlen(*tdriver) + 1;
                    ddir = malloc(ddir_size);
                    zbx_strlcpy(ddir, stat_dir, ddir_size);
                    zbx_strlcat(ddir, cgroup, ddir_size);
                    zbx_strlcat(ddir, *tdriver, ddir_size);
                    zabbix_log(LOG_LEVEL_DEBUG, "ddir to test: %s", ddir);

                    if (NULL != (dir = opendir(ddir)))
                    {
                        closedir(dir);
                        free(ddir);
                        driver = *tdriver;
                        zabbix_log(LOG_LEVEL_DEBUG, "Detected used LXD driver dir: %s", driver);
                        
                        // detect cpu_cgroup - JoinController cpu,cpuacct
                        cgroup = "cpu,cpuacct/";
                        ddir_size = strlen(cgroup) + strlen(stat_dir) + 1;
                        ddir = malloc(ddir_size);
                        zbx_strlcpy(ddir, stat_dir, ddir_size);
                        zbx_strlcat(ddir, cgroup, ddir_size);
                        if (NULL != (dir = opendir(ddir)))
                        {
                            closedir(dir);
                            cpu_cgroup = "cpu,cpuacct/";
                            zabbix_log(LOG_LEVEL_DEBUG, "Detected JoinController cpu,cpuacct");
                        } else {
                            cpu_cgroup = "cpuacct/";
                        }
                        free(ddir);
                        return SYSINFO_RET_OK;
                    }
                    *tdriver++;
                    free(ddir);
                }
                driver = "";
                zabbix_log(LOG_LEVEL_DEBUG, "Cannot detect used LXD driver");
                return SYSINFO_RET_FAIL;
            }
        }
        pclose(fp);
        zabbix_log(LOG_LEVEL_DEBUG, "Cannot detect LXD stat directory");
        return SYSINFO_RET_FAIL;
}

/******************************************************************************
 *                                                                            *
 * Function: zbx_module_lxd_up                                                *
 *                                                                            *
 * Purpose: check if container is running                                     *
 *                                                                            *
 * Return value: 1 - is running, 0 - is not running                           *
 *                                                                            *
 ******************************************************************************/
int     zbx_module_lxd_up(AGENT_REQUEST *request, AGENT_RESULT *result)
{
        zabbix_log(LOG_LEVEL_DEBUG, "In zbx_module_lxd_up()");
        char    *container;

        if (1 != request->nparam)
        {
                zabbix_log(LOG_LEVEL_ERR, "Invalid number of parameters: %d",  request->nparam);
                SET_MSG_RESULT(result, strdup("Invalid number of parameters"));
                return SYSINFO_RET_FAIL;
        }

        if (stat_dir == NULL || driver == NULL)
        {
                zabbix_log(LOG_LEVEL_DEBUG, "up check is not available at the moment - no stat directory");
                SET_MSG_RESULT(result, zbx_strdup(NULL, "up check is not available at the moment - no stat directory"));
                return SYSINFO_RET_FAIL;
        }

        if(cpu_cgroup == NULL)
        {
                if (zbx_lxd_dir_detect() == SYSINFO_RET_FAIL)
                {
                    zabbix_log(LOG_LEVEL_DEBUG, "up check is not available at the moment - no cpu_cgroup directory");
                    SET_MSG_RESULT(result, zbx_strdup(NULL, "up check is not available at the moment - no cpu_cgroup directory"));
                    return SYSINFO_RET_FAIL;
                }
        }

        container = zbx_strdup(NULL, get_rparam(request, 0));
        char    *stat_file = "/cpuacct.stat";
        char    *cgroup = cpu_cgroup;
        size_t  filename_size = strlen(cgroup) + strlen(container) + strlen(stat_dir) + strlen(driver) + strlen(stat_file) + 2;
        char    *filename = malloc(filename_size);
        zbx_strlcpy(filename, stat_dir, filename_size);
        zbx_strlcat(filename, cgroup, filename_size);
        zbx_strlcat(filename, driver, filename_size);
        zbx_strlcat(filename, container, filename_size);
        free(container);
        zbx_strlcat(filename, stat_file, filename_size);
        zabbix_log(LOG_LEVEL_DEBUG, "Metric source file: %s", filename);

        FILE    *file;
        if (NULL == (file = fopen(filename, "r")))
        {
                zabbix_log(LOG_LEVEL_DEBUG, "Cannot open metric file: '%s', container doesn't run", filename);
                free(filename);
                SET_UI64_RESULT(result, 0);
                return SYSINFO_RET_OK;
        }
        zbx_fclose(file);
        zabbix_log(LOG_LEVEL_DEBUG, "Can open metric file: '%s', container is running", filename);
        free(filename);
        SET_UI64_RESULT(result, 1);
        return SYSINFO_RET_OK;
}

/******************************************************************************
 *                                                                            *
 * Function: zbx_module_lxd_mem                                               *
 *                                                                            *
 * Purpose: container memory metrics                                          *
 *                                                                            *
 * Return value: SYSINFO_RET_FAIL - function failed, item will be marked      *
 *                                 as not supported by zabbix                 *
 *               SYSINFO_RET_OK - success                                     *
 *                                                                            *
 * Notes: https://www.kernel.org/doc/Documentation/cgroup-v1/memory.txt       *
 ******************************************************************************/
int     zbx_module_lxd_mem(AGENT_REQUEST *request, AGENT_RESULT *result)
{
        zabbix_log(LOG_LEVEL_DEBUG, "In zbx_module_lxd_mem()");
        char    *container, *metric;
        int     ret = SYSINFO_RET_FAIL;

        if (2 != request->nparam)
        {
                zabbix_log(LOG_LEVEL_ERR, "Invalid number of parameters: %d",  request->nparam);
                SET_MSG_RESULT(result, strdup("Invalid number of parameters"));
                return SYSINFO_RET_FAIL;
        }

        if (stat_dir == NULL || driver == NULL)
        {
                zabbix_log(LOG_LEVEL_DEBUG, "mem metrics are not available at the moment - no stat directory");
                SET_MSG_RESULT(result, zbx_strdup(NULL, "mem metrics are not available at the moment - no stat directory"));
                return SYSINFO_RET_FAIL;
        }

        container = zbx_strdup(NULL, get_rparam(request, 0));
        metric = get_rparam(request, 1);
        char    *stat_file = "/memory.stat";
        char    *cgroup = "memory/";
        size_t  filename_size = strlen(cgroup) + strlen(container) + strlen(stat_dir) + strlen(driver) + strlen(stat_file) + 2;
        char    *filename = malloc(filename_size);
        zbx_strlcpy(filename, stat_dir, filename_size);
        zbx_strlcat(filename, cgroup, filename_size);
        zbx_strlcat(filename, driver, filename_size);
        zbx_strlcat(filename, container, filename_size);
        zbx_strlcat(filename, stat_file, filename_size);
        zabbix_log(LOG_LEVEL_DEBUG, "Metric source file: %s", filename);
        
        FILE    *file;
        if (NULL == (file = fopen(filename, "r")))
        {
                zabbix_log(LOG_LEVEL_ERR, "Cannot open metric file: '%s'", filename);
                free(container);
                free(filename);
                SET_MSG_RESULT(result, strdup("Cannot open memory.stat file"));
                return SYSINFO_RET_FAIL;
        }

        char    line[MAX_STRING_LEN];
        char    *metric2 = malloc(strlen(metric)+3);
        memcpy(metric2, metric, strlen(metric));
        memcpy(metric2 + strlen(metric), " ", 2);
        zbx_uint64_t    value = 0;
        zabbix_log(LOG_LEVEL_DEBUG, "Looking metric %s in memory.stat file", metric);
        while (NULL != fgets(line, sizeof(line), file))
        {
                if (0 != strncmp(line, metric2, strlen(metric2)))
                        continue;
                if (1 != sscanf(line, "%*s " ZBX_FS_UI64, &value))
                {
                        zabbix_log(LOG_LEVEL_ERR, "sscanf failed for matched metric line");
                        continue;
                }
                zabbix_log(LOG_LEVEL_DEBUG, "Id: %s; metric: %s; value: %d", container, metric, value);
                SET_UI64_RESULT(result, value);
                ret = SYSINFO_RET_OK;
                break;
        }
        zbx_fclose(file);

        free(container);
        free(filename);
        free(metric2);

        if (SYSINFO_RET_FAIL == ret)
                SET_MSG_RESULT(result, zbx_strdup(NULL, "Cannot find a line with requested metric in memory.stat file"));

        return ret;
}

/******************************************************************************
 *                                                                            *
 * Function: zbx_module_lxd_cpu                                               *
 *                                                                            *
 * Purpose: container CPU metrics                                             *
 *                                                                            *
 * Return value: SYSINFO_RET_FAIL - function failed, item will be marked      *
 *                                 as not supported by zabbix                 *
 *               SYSINFO_RET_OK - success                                     *
 *                                                                            *
 * Notes: https://www.kernel.org/doc/Documentation/cgroup-v1/cpuacct.txt      *
 ******************************************************************************/
int     zbx_module_lxd_cpu(AGENT_REQUEST *request, AGENT_RESULT *result)
{
        zabbix_log(LOG_LEVEL_DEBUG, "In zbx_module_lxd_cpu()");

        char    *container, *metric;
        int     ret = SYSINFO_RET_FAIL;

        if (2 != request->nparam)
        {
                zabbix_log(LOG_LEVEL_ERR, "Invalid number of parameters: %d",  request->nparam);
                SET_MSG_RESULT(result, strdup("Invalid number of parameters"));
                return SYSINFO_RET_FAIL;
        }

        if (stat_dir == NULL || driver == NULL)
        {
                zabbix_log(LOG_LEVEL_DEBUG, "cpu metrics are not available at the moment - no stat directory");
                SET_MSG_RESULT(result, zbx_strdup(NULL, "cpu metrics are not available at the moment - no stat directory"));
                return SYSINFO_RET_FAIL;
        }

        if(cpu_cgroup == NULL)
        {
                if (zbx_lxd_dir_detect() == SYSINFO_RET_FAIL)
                {
                    zabbix_log(LOG_LEVEL_DEBUG, "cpu check is not available at the moment - no cpu_cgroup directory");
                    SET_MSG_RESULT(result, zbx_strdup(NULL, "cpu check is not available at the moment - no cpu_cgroup directory"));
                    return SYSINFO_RET_FAIL;
                }
        }

        container = zbx_strdup(NULL, get_rparam(request, 0));
        metric = get_rparam(request, 1);
        char    *cgroup = NULL, *stat_file = NULL;
        if(strcmp(metric, "user") == 0 || strcmp(metric, "system") == 0) {
            stat_file = "/cpuacct.stat";
            cgroup = cpu_cgroup;
        } else {
            stat_file = "/cpu.stat";
            if (strchr(cpu_cgroup, ',') != NULL) {
                cgroup = cpu_cgroup;
            } else {
                cgroup = "cpu/";
            }
        }

        zabbix_log(LOG_LEVEL_DEBUG, "cpu_cgroup: %s, cgroup: %s, stat_file: %s, metric: %s, container: %s", cpu_cgroup, cgroup, stat_file, metric, container);
        size_t  filename_size = strlen(cgroup) + strlen(container) + strlen(stat_dir) + strlen(driver) + strlen(stat_file) + 2;
        char    *filename = malloc(filename_size);
       
        zbx_strlcpy(filename, stat_dir, filename_size);
        zbx_strlcat(filename, cgroup, filename_size);
        zbx_strlcat(filename, driver, filename_size);
        zbx_strlcat(filename, container, filename_size);
        zbx_strlcat(filename, stat_file, filename_size);
        zabbix_log(LOG_LEVEL_DEBUG, "Metric source file: %s", filename);
        
        FILE    *file;
        if (NULL == (file = fopen(filename, "r")))
        {
                zabbix_log(LOG_LEVEL_ERR, "Cannot open metric file: '%s'", filename);
                free(filename);
                free(container);
                SET_MSG_RESULT(result, zbx_dsprintf(NULL, "Cannot open %s file", ++stat_file));
                return SYSINFO_RET_FAIL;
        }

        char    line[MAX_STRING_LEN];
        char    *metric2 = malloc(strlen(metric)+3);
        zbx_uint64_t cpu_num;
        memcpy(metric2, metric, strlen(metric));
        memcpy(metric2 + strlen(metric), " ", 2);
        zbx_uint64_t    value = 0;
        zabbix_log(LOG_LEVEL_DEBUG, "Looking metric %s in cpuacct.stat file", metric);
        while (NULL != fgets(line, sizeof(line), file))
        {
                
                if (0 != strncmp(line, metric2, strlen(metric2)))
                        continue;
                if (1 != sscanf(line, "%*s " ZBX_FS_UI64, &value))
                {
                        zabbix_log(LOG_LEVEL_ERR, "sscanf failed for matched metric line");
                        continue;
                }
                // normalize CPU usage by using number of online CPUs
                if (1 < (cpu_num = sysconf(_SC_NPROCESSORS_ONLN)))
                {
                    value /= cpu_num;
                }
                zabbix_log(LOG_LEVEL_DEBUG, "Id: %s; metric: %s; value: %d", container, metric, value);
                SET_UI64_RESULT(result, value);
                ret = SYSINFO_RET_OK;
                break;
        }
        zbx_fclose(file);

        free(container);
        free(filename);
        free(metric2);

        if (SYSINFO_RET_FAIL == ret)
                SET_MSG_RESULT(result, zbx_strdup(NULL, "Cannot find a line with requested metric in cpuacct.stat file"));

        return ret;
}

/******************************************************************************
 *                                                                            *
 * Function: zbx_module_lxd_dev                                               *
 *                                                                            *
 * Purpose: container device blkio metrics                                    *
 *                                                                            *
 * Return value: SYSINFO_RET_FAIL - function failed, item will be marked      *
 *                                 as not supported by zabbix                 *
 *               SYSINFO_RET_OK - success                                     *
 *                                                                            *
 * Notes: https://www.kernel.org/doc/Documentation/cgroup-v1/blkio-controller.txt
 ******************************************************************************/
int     zbx_module_lxd_dev(AGENT_REQUEST *request, AGENT_RESULT *result)
{
        zabbix_log(LOG_LEVEL_DEBUG, "In zbx_module_lxd_dev()");
        char    *container, *metric;
        int     ret = SYSINFO_RET_FAIL;

        if (3 != request->nparam)
        {
                zabbix_log(LOG_LEVEL_ERR, "Invalid number of parameters: %d",  request->nparam);
                SET_MSG_RESULT(result, strdup("Invalid number of parameters"));
                return SYSINFO_RET_FAIL;
        }

        if (stat_dir == NULL || driver == NULL)
        {
                zabbix_log(LOG_LEVEL_DEBUG, "dev metrics are not available at the moment - no stat directory");
                SET_MSG_RESULT(result, zbx_strdup(NULL, "dev metrics are not available at the moment - no stat directory"));
                return SYSINFO_RET_FAIL;
        }

        container = zbx_strdup(NULL, get_rparam(request, 0));
        char    *stat_file = malloc(strlen(get_rparam(request, 1)) + 2);
        zbx_strlcpy(stat_file, "/", strlen(get_rparam(request, 1)) + 2);
        zbx_strlcat(stat_file, get_rparam(request, 1), strlen(get_rparam(request, 1)) + 2);
        metric = get_rparam(request, 2);

        char    *cgroup = "blkio/";
        size_t  filename_size = strlen(cgroup) + strlen(container) + strlen(stat_dir) + strlen(driver) + strlen(stat_file) + 2;
        char    *filename = malloc(filename_size);
        zbx_strlcpy(filename, stat_dir, filename_size);
        zbx_strlcat(filename, cgroup, filename_size);
        zbx_strlcat(filename, driver, filename_size);
        zbx_strlcat(filename, container, filename_size);
        zbx_strlcat(filename, stat_file, filename_size);
        zabbix_log(LOG_LEVEL_DEBUG, "Metric source file: %s", filename);

        FILE    *file;
        if (NULL == (file = fopen(filename, "r")))
        {
                zabbix_log(LOG_LEVEL_ERR, "Cannot open metric file: '%s'", filename);
                free(container);
                free(stat_file);
                free(filename);
                SET_MSG_RESULT(result, strdup("Cannot open stat file, maybe CONFIG_DEBUG_BLK_CGROUP is not enabled"));
                zabbix_log(LOG_LEVEL_ERR, "Cannot open stat file, maybe CONFIG_DEBUG_BLK_CGROUP is not enabled");
                return SYSINFO_RET_FAIL;
        }

        char    line[MAX_STRING_LEN];
        char    *metric2 = malloc(strlen(metric)+3);
        memcpy(metric2, metric, strlen(metric));
        memcpy(metric2 + strlen(metric), " ", 2);
        zbx_uint64_t    value = 0;
        zabbix_log(LOG_LEVEL_DEBUG, "Looking metric %s in blkio file", metric);
        while (NULL != fgets(line, sizeof(line), file))
        {
                if (0 != strncmp(line, metric2, strlen(metric2)))
                        continue;
                if (1 != sscanf(line, "%*s " ZBX_FS_UI64, &value))
                {
                     // maybe per blk device metric, e.g. '8:0 Read'
                     if (1 != sscanf(line, "%*s %*s " ZBX_FS_UI64, &value))
                     {
                         zabbix_log(LOG_LEVEL_ERR, "sscanf failed for matched metric line");
                         break;
                     }
                }
                zabbix_log(LOG_LEVEL_DEBUG, "Id: %s; stat file: %s, metric: %s; value: %d", container, stat_file, metric, value);
                SET_UI64_RESULT(result, value);
                ret = SYSINFO_RET_OK;
                break;
        }
        zbx_fclose(file);

        free(container);
        free(stat_file);
        free(filename);
        free(metric2);

        if (SYSINFO_RET_FAIL == ret){
                SET_MSG_RESULT(result, zbx_strdup(NULL, "Cannot find a line with requested metric in blkio file"));
                zabbix_log(LOG_LEVEL_ERR, "Cannot find a line with requested metric in blkio file");
        }

        return ret;
}


/******************************************************************************
 *                                                                            *
 * Function: zbx_module_lxd_disk                                               *
 *                                                                            *
 * Purpose: container disk metrics                                            *
 *                                                                            *
 * Return value: SYSINFO_RET_FAIL - function failed, item will be marked      *
 *                                 as not supported by zabbix                 *
 *               SYSINFO_RET_OK - success                                     *
 *                                                                            *
 ******************************************************************************/

int     zbx_module_lxd_disk(AGENT_REQUEST *request, AGENT_RESULT *result)
{
        zabbix_log(LOG_LEVEL_DEBUG, "In zbx_module_lxd_disk()");
        char    *container, *metric;
        int     ret = SYSINFO_RET_FAIL;
        FILE        *fp;
        char        buf[128];

        if (2 != request->nparam)
        {
                zabbix_log(LOG_LEVEL_ERR, "Invalid number of parameters: %d",  request->nparam);
                SET_MSG_RESULT(result, strdup("Invalid number of parameters"));
                return SYSINFO_RET_FAIL;
        }

        if (stat_dir == NULL || driver == NULL)
        {
                zabbix_log(LOG_LEVEL_DEBUG, "disk metrics are not available at the moment - no stat directory");
                SET_MSG_RESULT(result, zbx_strdup(NULL, "disk metrics are not available at the moment - no stat directory"));
                return SYSINFO_RET_FAIL;
        }

        if(cpu_cgroup == NULL)
        {
                if (zbx_lxd_dir_detect() == SYSINFO_RET_FAIL)
                {
                    zabbix_log(LOG_LEVEL_DEBUG, "disk check is not available at the moment - no cpu_cgroup directory");
                    SET_MSG_RESULT(result, zbx_strdup(NULL, "disk check is not available at the moment - no cpu_cgroup directory"));
                    return SYSINFO_RET_FAIL;
                }
        }

        container = zbx_strdup(NULL, get_rparam(request, 0));
        metric = get_rparam(request, 1);
        char    *stat_file = "/ns/init.scope/cgroup.procs";
        size_t  filename_size = strlen(cpu_cgroup) + strlen(container) + strlen(stat_dir) + strlen(driver) + strlen(stat_file) + 2;
        char    *filename = malloc(filename_size);
        zbx_strlcpy(filename, stat_dir, filename_size);
        zbx_strlcat(filename, cpu_cgroup, filename_size);
        zbx_strlcat(filename, driver, filename_size);
        zbx_strlcat(filename, container, filename_size);
        zbx_strlcat(filename, stat_file, filename_size);
        zabbix_log(LOG_LEVEL_DEBUG, "Metric source file: %s", filename);
        metric = get_rparam(request, 1);

        char *pid = NULL;
        if ((fp = fopen(filename, "r")) != NULL) {
                while(fgets(buf, 128, fp) != NULL) {
                        int pid_t;
                               if (1 == sscanf(buf, ZBX_FS_UI64, &pid_t)) {
                                size_t size = (int)ceil(log10(abs(pid_t))) + 1;
                                pid = malloc(size);
                                zbx_snprintf(pid, size, "%d", pid_t);
                        }
                }

                zbx_fclose(fp);
        } else {
                zabbix_log(LOG_LEVEL_DEBUG, "Cannot open metric file");
        }

        if (pid == NULL) {
                FILE *popen (char *argv[]) {
                        int fd[2];
                        pid_t pid;
                        int status;

                        if (pipe(fd) < 0) {
                                return NULL;
                        } else if ((pid = fork()) < 0) {
                                return NULL;
                        } else if (pid == 0) {
                                close(fd[0]);
                                dup2(fd[1], STDOUT_FILENO);
                                dup2(fd[1], STDERR_FILENO);
                                close(fd[1]);

                                setenv("HOME", "/tmp/", 0);
                                execvp(argv[0], argv);
                                exit(-1);
                        } else {
                                close(fd[1]);
                                return fdopen(fd[0], "r");
                        }
                }

                // Find the process id for the lxc container
                char *lxc_cmd[8] = {"lxc-info", "-n", container, "-p", "-P", "/var/lib/lxc"};

                if ((fp = popen(lxc_cmd)) == NULL) {
                        zabbix_log(LOG_LEVEL_ERR, "Invoking lxc-info failed");
                        SET_MSG_RESULT(result, strdup("Invoking lxc-info failed"));
                        free(container);
                        return ret;
                }

                while(fgets(buf, 128, fp) != NULL) {
                        char *pid_s;
                        if ((pid_s = strtok(buf, "PID:")) != NULL && buf != pid_s) {
                                int pid_t;
                                       if (1 == sscanf(buf, "%*s " ZBX_FS_UI64, &pid_t)) {
                                        size_t size = (int)ceil(log10(abs(pid_t))) + 1;
                                        pid = malloc(size);
                                        zbx_snprintf(pid, size, "%d", pid_t);
                                }
                        }
                }

                if (pclose(fp)) {
                        zabbix_log(LOG_LEVEL_ERR, "lxc-info returned error status");
                        SET_MSG_RESULT(result, strdup("lxc-info returned error status"));
                        free(container);
                        free(pid);
                        return ret;
                }
        }

        if (pid == NULL) {
                zabbix_log(LOG_LEVEL_ERR, "Could not find process id, container %s", container);
                SET_MSG_RESULT(result, strdup("Could not find process id"));
                free(container);
                return ret;
        }

        // Find the disk usage of the /proc/$PID/root folder
        size_t root_size = 12 + strlen(pid) + 1;
        char *root_path = malloc(root_size);
        zbx_strlcpy(root_path, "/proc/", root_size);
        zbx_strlcat(root_path, pid, root_size);
        zbx_strlcat(root_path, "/root/", root_size);

        struct statvfs rootfs_stats;
        if (statvfs(root_path, &rootfs_stats) == -1) {
                zabbix_log(LOG_LEVEL_ERR, "Failed to retrieve stats for process root, %d", errno);
                SET_MSG_RESULT(result, strdup("Failed to retrieve stats for process root"));
                free(container);
                free(root_path);
                return SYSINFO_RET_FAIL;
        }

        unsigned long long int value;
        double falue;
        if (0 == strcmp(metric, "size")) {
                value = rootfs_stats.f_frsize * rootfs_stats.f_blocks;
                zabbix_log(LOG_LEVEL_DEBUG, "Id: %s; metric: %s; value: %llu", container, metric, value);
                SET_UI64_RESULT(result, value);
                ret = SYSINFO_RET_OK;
        } else if (0 == strcmp(metric, "free")) {
                value = rootfs_stats.f_bsize * rootfs_stats.f_bfree;
                zabbix_log(LOG_LEVEL_DEBUG, "Id: %s; metric: %s; value: %llu", container, metric, value);
                SET_UI64_RESULT(result, value);
                ret = SYSINFO_RET_OK;
        } else if (0 == strcmp(metric, "pfree")) {
                falue = 100 * (double)(rootfs_stats.f_bsize * rootfs_stats.f_bfree) / (rootfs_stats.f_frsize * rootfs_stats.f_blocks);
                zabbix_log(LOG_LEVEL_DEBUG, "Id: %s; metric: %s; value: %lf", container, metric, value);
                SET_DBL_RESULT(result, falue);
                ret = SYSINFO_RET_OK;
        } else if (0 == strcmp(metric, "avail")) {
                value = rootfs_stats.f_bsize * rootfs_stats.f_bavail;
                zabbix_log(LOG_LEVEL_DEBUG, "Id: %s; metric: %s; value: %llu", container, metric, value);
                SET_UI64_RESULT(result, value);
                ret = SYSINFO_RET_OK;
        } else if (0 == strcmp(metric, "pavail")) {
                falue = 100 * (double)(rootfs_stats.f_bsize * rootfs_stats.f_bfree) / (rootfs_stats.f_frsize * rootfs_stats.f_blocks);
                zabbix_log(LOG_LEVEL_DEBUG, "Id: %s; metric: %s; value: %llf", container, metric, value);
                SET_DBL_RESULT(result, falue);
                ret = SYSINFO_RET_OK;
        } else if (0 == strcmp(metric, "inodes")) {
                value = rootfs_stats.f_files;
                zabbix_log(LOG_LEVEL_DEBUG, "Id: %s; metric: %s; value: %llu", container, metric, value);
                SET_UI64_RESULT(result, value);
                ret = SYSINFO_RET_OK;
        } else if (0 == strcmp(metric, "inodes_free")) {
                value = rootfs_stats.f_ffree;
                zabbix_log(LOG_LEVEL_DEBUG, "Id: %s; metric: %s; value: %llu", container, metric, value);
                SET_UI64_RESULT(result, value);
                ret = SYSINFO_RET_OK;
        } else if (0 == strcmp(metric, "inodes_pfree")) {
                falue = 100 * (double)rootfs_stats.f_ffree / rootfs_stats.f_files;;
                zabbix_log(LOG_LEVEL_DEBUG, "Id: %s; metric: %s; value: %llf", container, metric, value);
                SET_DBL_RESULT(result, falue);
                ret = SYSINFO_RET_OK;
        } else if (0 == strcmp(metric, "inodes_avail")) {
                value = rootfs_stats.f_favail;
                zabbix_log(LOG_LEVEL_DEBUG, "Id: %s; metric: %s; value: %llu", container, metric, value);
                SET_UI64_RESULT(result, value);
                ret = SYSINFO_RET_OK;
        } else if (0 == strcmp(metric, "inodes_pavail")) {
                falue = 100 * (double)rootfs_stats.f_favail / rootfs_stats.f_files;;
                zabbix_log(LOG_LEVEL_DEBUG, "Id: %s; metric: %s; value: %llf", container, metric, value);
                SET_DBL_RESULT(result, falue);
                ret = SYSINFO_RET_OK;
        } else {
                zabbix_log(LOG_LEVEL_ERR, "Unknown metric:: %s", metric);
                SET_MSG_RESULT(result, strdup("Unknown metric"));
        }

        free(container);
        free(root_path);
        if (pid != NULL) free(pid);

        return ret;
}

/******************************************************************************
 *                                                                            *
 * Function: zbx_module_uninit                                                *
 *                                                                            *
 * Purpose: the function is called on agent shutdown                          *
 *          It should be used to cleanup used resources if there are any      *
 *                                                                            *
 * Return value: ZBX_MODULE_OK - success                                      *
 *               ZBX_MODULE_FAIL - function failed                            *
 *                                                                            *
 ******************************************************************************/
int     zbx_module_uninit()
{
        zabbix_log(LOG_LEVEL_DEBUG, "In zbx_module_uninit()");

        const char* znetns_prefix = "zabbix_module_lxd_";
        DIR             *dir;
        struct dirent   *d;

        if (NULL == (dir = opendir("/var/run/netns")))
        {
            zabbix_log(LOG_LEVEL_DEBUG, "/var/run/netns: %s", zbx_strerror(errno));
            return ZBX_MODULE_OK;
        }
        char *file = NULL;
        while (NULL != (d = readdir(dir)))
        {
            if(0 == strcmp(d->d_name, ".") || 0 == strcmp(d->d_name, ".."))
                continue;

            // delete zabbix netns
            if ((strstr(d->d_name, znetns_prefix)) != NULL)
            {
                file = NULL;
                file = zbx_dsprintf(file, "/var/run/netns/%s", d->d_name);
                if(unlink(file) != 0)
                {
                    zabbix_log(LOG_LEVEL_WARNING, "%s: %s", d->d_name, zbx_strerror(errno));
                }
            }
        }
        if(0 != closedir(dir))
        {
            zabbix_log(LOG_LEVEL_WARNING, "/var/run/netns/: %s", zbx_strerror(errno));
        }

        free(stat_dir);

        return ZBX_MODULE_OK;
}

/******************************************************************************
 *                                                                            *
 * Function: zbx_module_init                                                  *
 *                                                                            *
 * Purpose: the function is called on agent startup                           *
 *          It should be used to call any initialization routines             *
 *                                                                            *
 * Return value: ZBX_MODULE_OK - success                                      *
 *               ZBX_MODULE_FAIL - module initialization failed               *
 *                                                                            *
 * Comment: the module won't be loaded in case of ZBX_MODULE_FAIL             *
 *                                                                            *
 ******************************************************************************/
int     zbx_module_init()
{
        zabbix_log(LOG_LEVEL_DEBUG, "In zbx_module_init()");
        zabbix_log(LOG_LEVEL_DEBUG, "zabbix_module_lxd %s, compilation time: %s %s", m_version, __DATE__, __TIME__);
        zbx_lxd_dir_detect();
        return ZBX_MODULE_OK;
}


/******************************************************************************
 *                                                                            *
 * Function: zbx_module_lxd_discovery                                         *
 *                                                                            *
 * Purpose: container discovery                                               *
 *                                                                            *
 * Return value: SYSINFO_RET_FAIL - function failed, item will be marked      *
 *                                 as not supported by zabbix                 *
 *               SYSINFO_RET_OK - success                                     *
 *                                                                            *
 ******************************************************************************/
int     zbx_module_lxd_discovery(AGENT_REQUEST *request, AGENT_RESULT *result)
{
        zabbix_log(LOG_LEVEL_DEBUG, "In zbx_module_lxd_discovery()");

        struct zbx_json j;
        if(stat_dir == NULL && zbx_lxd_dir_detect() == SYSINFO_RET_FAIL)
        {
            zabbix_log(LOG_LEVEL_DEBUG, "lxd.discovery is not available at the moment - no stat directory - empty discovery");
            zbx_json_init(&j, ZBX_JSON_STAT_BUF_LEN);
            zbx_json_addarray(&j, ZBX_PROTO_TAG_DATA);
            zbx_json_close(&j);
            SET_STR_RESULT(result, zbx_strdup(NULL, j.buffer));
            zbx_json_free(&j);
            return SYSINFO_RET_FAIL;
        }

        DIR             *dir;
        zbx_stat_t      sb;
        char            *file = NULL, *containerid;
        struct dirent   *d;
        char    *cgroup = "cpuset/";
        size_t  ddir_size = strlen(cgroup) + strlen(stat_dir) + strlen(driver) + 2;
        char    *ddir = malloc(ddir_size);
        zbx_strlcpy(ddir, stat_dir, ddir_size);
        zbx_strlcat(ddir, cgroup, ddir_size);
        zbx_strlcat(ddir, driver, ddir_size);
        zabbix_log(LOG_LEVEL_DEBUG, "lxd.discovery-> ddir: %s", ddir);

        if (NULL == (dir = opendir(ddir)))
        {
            zabbix_log(LOG_LEVEL_WARNING, "%s: %s", ddir, zbx_strerror(errno));
            free(ddir);
            return SYSINFO_RET_FAIL;
        }

        zbx_json_init(&j, ZBX_JSON_STAT_BUF_LEN);
        zbx_json_addarray(&j, ZBX_PROTO_TAG_DATA);

        size_t hostname_len = 128;
        while (1) {
            char* realloc_hostname = realloc(hostname, hostname_len);
            if (realloc_hostname == 0) {
                free(hostname);
            }
            hostname = realloc_hostname;
            hostname[hostname_len-1] = 0;
            if (gethostname(hostname, hostname_len-1) == 0) {
                size_t count = strlen(hostname);
                if (count < hostname_len-2) {
                    break;
                }
            }
            hostname_len *= 2;
        }
        zabbix_log(LOG_LEVEL_WARNING, "hostname: %s, dir: %s", hostname, ddir);
        while (NULL != (d = readdir(dir)))
        {
                if(0 == strcmp(d->d_name, ".") || 0 == strcmp(d->d_name, ".."))
                        continue;

                file = zbx_dsprintf(file, "%s/%s", ddir, d->d_name);

                if (0 != zbx_stat(file, &sb) || 0 == S_ISDIR(sb.st_mode))
                        continue;

                containerid = d->d_name;

                zbx_json_addobject(&j, NULL);
                zbx_json_addstring(&j, "{#HCONTAINERID}", containerid, ZBX_JSON_TYPE_STRING);
                zbx_json_addstring(&j, "{#SYSTEM.HOSTNAME}", hostname, ZBX_JSON_TYPE_STRING);
                zbx_json_close(&j);

        }

        if(0 != closedir(dir))
        {
            zabbix_log(LOG_LEVEL_WARNING, "%s: %s\n", ddir, zbx_strerror(errno));
        }

        zbx_json_close(&j);

        SET_STR_RESULT(result, zbx_strdup(NULL, j.buffer));

        zbx_json_free(&j);

        free(ddir);

        return SYSINFO_RET_OK;
}

