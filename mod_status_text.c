/* Licensed to the Apache Software Foundation (ASF) under one or more
 * contributor license agreements.  See the NOTICE file distributed with
 * this work for additional information regarding copyright ownership.
 * The ASF licenses this file to You under the Apache License, Version 2.0
 * (the "License"); you may not use this file except in compliance with
 * the License.  You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/* Status Module.  Display lots of internal data about how Apache is
 * performing and the state of all children processes.
 *
 * Original source by Mark Cox, mark@ukweb.com, November 1995
 * V1.0 by Mathieu CARBONNEAUX, 06/08/2010, Full Text fork for only machine automatique parsing
 * V1.1 by Mathieu CARBONNEAUX, 22/10/2010, add realtime log statistique
 * V1.2 by Mathieu CARBONNEAUX, 27/10/2013, add httpd 2.4 support
 */

#define CORE_PRIVATE
#include "httpd.h"
#include "http_config.h"
#include "http_core.h"
#include "http_protocol.h"
#include "http_main.h"
#include "ap_mpm.h"
#include "util_script.h"
#include <time.h>
#include "http_log.h"
#include "mod_status.h"
#include "ap_listen.h"
#if APR_HAVE_UNISTD_H
#include <unistd.h>
#endif
#include "apr_version.h"
#include "apu_version.h"
#define APR_WANT_STRFUNC
#include "apr_want.h"
#include "apr_strings.h"
#include "mod_status_text_config.h"
#include "scoreboard.h"

#ifdef NEXT
#if (NX_CURRENT_COMPILER_RELEASE == 410)
#ifdef m68k
#define HZ 64
#else
#define HZ 100
#endif
#else
#include <machine/param.h>
#endif
#endif /* NEXT */

#define STATUS_MAGIC_TYPE "application/x-httpd-status-text"
#define KBYTE 1024
#define MOD_VERSION "1.2.2"

module AP_MODULE_DECLARE_DATA status_text_module;

/* status text scoreboard */
typedef struct {
    /* number of request per status code */
    apr_uint64_t nb_reqs_xxx;
    apr_uint64_t nb_reqs_10x;
    apr_uint64_t nb_reqs_200;
    apr_uint64_t nb_reqs_20x;
    apr_uint64_t nb_reqs_301;
    apr_uint64_t nb_reqs_302;
    apr_uint64_t nb_reqs_304;
    apr_uint64_t nb_reqs_30x;
    apr_uint64_t nb_reqs_404;
    apr_uint64_t nb_reqs_40x;
    apr_uint64_t nb_reqs_50x;

    /* number of bytes per status code */
    apr_uint64_t nb_bytes_xxx;
    apr_uint64_t nb_bytes_10x;
    apr_uint64_t nb_bytes_200;
    apr_uint64_t nb_bytes_20x;
    apr_uint64_t nb_bytes_301;
    apr_uint64_t nb_bytes_302;
    apr_uint64_t nb_bytes_304;
    apr_uint64_t nb_bytes_30x;
    apr_uint64_t nb_bytes_404;
    apr_uint64_t nb_bytes_40x;
    apr_uint64_t nb_bytes_50x;

    /* number of request per response times range */
    apr_time_t nb_reqs_50ms;  /* 0ms to 50ms range */
    apr_time_t nb_reqs_100ms; /* 51ms to 100ms range */
    apr_time_t nb_reqs_300ms; /* 101ms to 300ms range */
    apr_time_t nb_reqs_500ms; /* 301ms to 500ms range */
    apr_time_t nb_reqs_1s;    /* 501ms to 1s range */
    apr_time_t nb_reqs_1_5s;  /* 1s to 1.5s range */
    apr_time_t nb_reqs_2s;    /* 1.5s to 2s range */
    apr_time_t nb_reqs_5s;    /* 2s to 5s range */
    apr_time_t nb_reqs_10s;   /* 5s to 10s range */
    apr_time_t nb_reqs_15s;   /* 10s to 15s range */
    apr_time_t nb_reqs_20s;   /* 15s to 20s range */
    apr_time_t nb_reqs_30s;   /* 20s to 30s range */
    apr_time_t nb_reqs_xs;    /* more than 30s range */

    /* response time statistiques... */
    apr_time_t last; /* last response time */
    apr_time_t avg; /* average response time */
    apr_time_t percentil; /* 90% percentil response time */

    apr_time_t first_trend;  /* first trend date */
    apr_time_t trend[10]; /* circular array used to calculate percentil */
    int trend_curpos; /* circular array current position */
} status_text_scoreboard_t; 

const char *status_text_scorebored_name = NULL;
apr_shm_t *status_text_scoreboard_shm = NULL;
status_text_scoreboard_t *status_text_scoreboard = NULL;

static int server_limit, thread_limit;
static int forked, threaded;
static apr_size_t status_text_scoreboard_size;

#ifdef HAVE_TIMES
/* ugh... need to know if we're running with a pthread implementation
 * such as linuxthreads that treats individual threads as distinct
 * processes; that affects how we add up CPU time in a process
 */
static pid_t child_pid;
#endif

/* hack to be abel to get child and thread number */
typedef struct  {
     int child_num;
     int thread_num;
} my_sb_handle_t;


static char status_text_flags[SERVER_NUM_STATUS];

static int runtime_statistique(request_rec *r)
{
    /* now time */
    apr_time_t now=apr_time_now();

    /* get scoreboard handle to get current thread and child number */
    my_sb_handle_t *sb=r->connection->sbh; 
    /* scoreboard current position */
    int sb_pos=sb->child_num * thread_limit + sb->thread_num;

    /* get status text scoreboard share memory */
    status_text_scoreboard_t *st_sb=&status_text_scoreboard[sb_pos];

    /* get the current worker score */
    /*worker_score *ws = &ap_scoreboard_image->servers[sb->child_num][sb->thread_num];*/

    /* request bytes sent */
    apr_uint64_t bytes=r->bytes_sent;

    /* request time duration */
    apr_time_t req_response_time=(now-r->request_time)/1000;

    if (req_response_time>0) 
    {
      /* update last response time */
      st_sb->last=req_response_time;

      /* update average response time */
      if (st_sb->avg!=0) st_sb->avg=(st_sb->avg+req_response_time)/2;
      else st_sb->avg=req_response_time;

      /* if trend is from more than 10s  ==> reinitialisze */
      if ((now-st_sb->first_trend)>(10*1000))
      {
         /* reinitialise with avg response time */
         st_sb->percentil=st_sb->avg;

         /* initialize first trend time */
         st_sb->first_trend=now;

	 /* reinit position */
	 st_sb->trend_curpos=0;

         /* restart to first position */
	 st_sb->trend[st_sb->trend_curpos++]=req_response_time;
      }

      /* update 90% perscentil response time */
      if (st_sb->trend_curpos<10)
      {
	 //int i;
	 st_sb->trend[st_sb->trend_curpos++]=req_response_time;
         //for(i=1;i<st_sb->trend_curpos;i++) st_sb->avg+=st_sb->trend[i];
         //st_sb->avg/=st_sb->trend_curpos;
      }
      else
      {
	 /* sort the trend array (insertion sort algo) */
	 int i;
	 int j;
	 for(i=1;i<10;i++)
	 {
	   apr_time_t x=st_sb->trend[i];
	   j=i;
	   while(j>0&&st_sb->trend[j-1]>x)
	   {
	     st_sb->trend[j]=st_sb->trend[j-1];
	     j=j-1;
	   }
	   st_sb->trend[j]=x;
	 }
	 /* get 90% centil */
	 st_sb->percentil=st_sb->trend[8];

         /* initialize first trend time */
         st_sb->first_trend=now;

	 /* reinit position */
	 st_sb->trend_curpos=0;
      }

      if (st_sb->percentil==0) 
      {
         st_sb->percentil=st_sb->avg;
         /* initialize first trend time */
         st_sb->first_trend=now;
      }
    }

    /* request response time distribution */
    if (req_response_time<=50)
      st_sb->nb_reqs_50ms++;  
    else if (req_response_time<=100)
      st_sb->nb_reqs_100ms++;  
    else if (req_response_time<=300)
      st_sb->nb_reqs_300ms++;  
    else if (req_response_time<=500)
      st_sb->nb_reqs_500ms++;  
    else if (req_response_time<=1*1000)
      st_sb->nb_reqs_1s++;  
    else if (req_response_time<=1.5*1000)
      st_sb->nb_reqs_1_5s++;  
    else if (req_response_time<=2*1000)
      st_sb->nb_reqs_2s++;  
    else if (req_response_time<=5*1000)
      st_sb->nb_reqs_5s++;  
    else if (req_response_time<=10*1000)
      st_sb->nb_reqs_10s++;  
    else if (req_response_time<=15*1000)
      st_sb->nb_reqs_15s++;  
    else if (req_response_time<=20*1000)
      st_sb->nb_reqs_20s++;  
    else if (req_response_time<30*1000)
      st_sb->nb_reqs_30s++;  
    else
      st_sb->nb_reqs_xs++;  

    /* number of requests/bytes per type of response status code */
    if (r->status>=500)
    {
      st_sb->nb_reqs_50x++;  
      st_sb->nb_bytes_50x+=bytes;  
    }
    else if (r->status>=400)
    {
      if (r->status==404)
      {
	st_sb->nb_reqs_404++;  
	st_sb->nb_bytes_404+=bytes;
      }
      else
      {
	st_sb->nb_reqs_40x++;  
	st_sb->nb_bytes_40x+=bytes;
      }
    }
    else if (r->status>=300)
    {
      if (r->status==301)
      {
	st_sb->nb_reqs_301++;  
	st_sb->nb_bytes_301+=bytes;
      }
      else if (r->status==302)
      {
	st_sb->nb_reqs_302++;  
	st_sb->nb_bytes_302+=bytes;
      }
      else if (r->status==304)
      {
	st_sb->nb_reqs_304++;  
	st_sb->nb_bytes_304+=bytes;
      }
      else
      {
	st_sb->nb_reqs_30x++;  
	st_sb->nb_bytes_30x+=bytes;
      }
    }
    else if (r->status>=200)
    {
      if (r->status==200)
      {
	st_sb->nb_reqs_200++;  
	st_sb->nb_bytes_200+=bytes;
      }
      else
      {
	st_sb->nb_reqs_20x++;  
	st_sb->nb_bytes_20x+=bytes;
      }
    }
    else if (r->status>=100)
    {
      st_sb->nb_reqs_10x++;  
      st_sb->nb_bytes_10x+=bytes;
    }
    else if (r->status<100)
    {
      st_sb->nb_reqs_xxx++;  
      st_sb->nb_bytes_xxx+=bytes;
    }
       
    return OK;
}

/* Main handler for x-httpd-status-text requests */
static int status_text_handler(request_rec *r)
{
    apr_time_t nowtime;
    apr_interval_time_t up_time;
    int max_daemons;
    int j, i, res;
    int lr_count=0;
    ap_listen_rec *lr=NULL;
    int ready;
    int busy;
    apr_uint64_t count;
    apr_uint64_t lres, my_lres, conn_lres;
    apr_uint64_t bytes, my_bytes, conn_bytes;
    apr_uint64_t bcount, kbcount;
    long req_time;
#ifdef HAVE_TIMES
    float tick;
    int times_per_thread = getpid() != child_pid;
#endif
    worker_score ws_record_st;
    worker_score *ws_record=&ws_record_st;
    process_score *ps_record;
    char *stat_buffer;
    pid_t *pid_buffer, worker_pid;
    clock_t tu, ts, tcu, tcs;
    ap_generation_t worker_generation;
    apr_time_t *percentil_array;
    unsigned long nb_avg=0;
    unsigned long nb_percentil=0;
    unsigned long percentilpos=0;

    /* get status text scoreboard share memory */
    status_text_scoreboard_t *st_sb=status_text_scoreboard;
    status_text_scoreboard_t st_sb_total;
    status_text_scoreboard_t *st_sb_cur;

    if (strcmp(r->handler, STATUS_MAGIC_TYPE) && strcmp(r->handler, "server-status-text")) 
        return DECLINED;

#ifdef HAVE_TIMES
#ifdef _SC_CLK_TCK
    tick = sysconf(_SC_CLK_TCK);
#else
    tick = HZ;
#endif
#endif

    pid_buffer = apr_palloc(r->pool, server_limit * sizeof(pid_t));
    stat_buffer = apr_palloc(r->pool, server_limit * thread_limit * sizeof(char));
    percentil_array = apr_palloc(r->pool, server_limit * thread_limit * sizeof(apr_time_t));

    ready = 0;
    busy = 0;
    count = 0;
    bcount = 0;
    kbcount = 0;
    memset(&st_sb_total,0,sizeof(status_text_scoreboard_t));
    memset(percentil_array,0,server_limit * thread_limit * sizeof(apr_time_t));

#if AP_MODULE_MAGIC_AT_LEAST(20090401,1)
    ap_generation_t ap_my_generation;
    ap_mpm_query(AP_MPMQ_GENERATION, &ap_my_generation);
#endif

    nowtime = apr_time_now();
    tu = ts = tcu = tcs = 0;

    if (!ap_exists_scoreboard_image()) 
    {
        ap_log_rerror(APLOG_MARK, APLOG_ERR, 0, r,
                      "Server status unavailable in inetd mode");
        return HTTP_INTERNAL_SERVER_ERROR;
    }

    r->allowed = (AP_METHOD_BIT << M_GET);
    if (r->method_number != M_GET)
        return DECLINED;

    ap_set_content_type(r, "text/plain; charset=ISO-8859-1");

    for (i = 0; i < server_limit; ++i) 
    {
#ifdef HAVE_TIMES
        clock_t proc_tu = 0, proc_ts = 0, proc_tcu = 0, proc_tcs = 0;
        clock_t tmp_tu, tmp_ts, tmp_tcu, tmp_tcs;
#endif
        ps_record = ap_get_scoreboard_process(i);
        for (j = 0; j < thread_limit; ++j) 
	{
            int indx = (i * thread_limit) + j;

	    /* get the current status text scoreboard */
	    st_sb_cur=&st_sb[indx];


            /* number of request per status code */
	    st_sb_total.nb_reqs_xxx+=st_sb_cur->nb_reqs_xxx;
	    st_sb_total.nb_reqs_10x+=st_sb_cur->nb_reqs_10x;
	    st_sb_total.nb_reqs_200+=st_sb_cur->nb_reqs_200;
	    st_sb_total.nb_reqs_20x+=st_sb_cur->nb_reqs_20x;
	    st_sb_total.nb_reqs_301+=st_sb_cur->nb_reqs_301;
	    st_sb_total.nb_reqs_302+=st_sb_cur->nb_reqs_302;
	    st_sb_total.nb_reqs_304+=st_sb_cur->nb_reqs_304;
	    st_sb_total.nb_reqs_30x+=st_sb_cur->nb_reqs_30x;
	    st_sb_total.nb_reqs_404+=st_sb_cur->nb_reqs_404;
	    st_sb_total.nb_reqs_40x+=st_sb_cur->nb_reqs_40x;
	    st_sb_total.nb_reqs_50x+=st_sb_cur->nb_reqs_50x;

	    /* number of bytes per status code */
	    st_sb_total.nb_bytes_xxx+=st_sb_cur->nb_bytes_xxx;
	    st_sb_total.nb_bytes_10x+=st_sb_cur->nb_bytes_10x;
	    st_sb_total.nb_bytes_200+=st_sb_cur->nb_bytes_200;
	    st_sb_total.nb_bytes_20x+=st_sb_cur->nb_bytes_20x;
	    st_sb_total.nb_bytes_301+=st_sb_cur->nb_bytes_301;
	    st_sb_total.nb_bytes_302+=st_sb_cur->nb_bytes_302;
	    st_sb_total.nb_bytes_304+=st_sb_cur->nb_bytes_304;
	    st_sb_total.nb_bytes_30x+=st_sb_cur->nb_bytes_30x;
	    st_sb_total.nb_bytes_404+=st_sb_cur->nb_bytes_404;
	    st_sb_total.nb_bytes_40x+=st_sb_cur->nb_bytes_40x;
	    st_sb_total.nb_bytes_50x+=st_sb_cur->nb_bytes_50x;

	    /* number of request per response times range */
	    st_sb_total.nb_reqs_50ms +=st_sb_cur->nb_reqs_50ms;
	    st_sb_total.nb_reqs_100ms+=st_sb_cur->nb_reqs_100ms;
	    st_sb_total.nb_reqs_300ms+=st_sb_cur->nb_reqs_300ms;
	    st_sb_total.nb_reqs_500ms+=st_sb_cur->nb_reqs_500ms;
	    st_sb_total.nb_reqs_1s   +=st_sb_cur->nb_reqs_1s;
	    st_sb_total.nb_reqs_1_5s +=st_sb_cur->nb_reqs_1_5s;
	    st_sb_total.nb_reqs_2s   +=st_sb_cur->nb_reqs_2s;
	    st_sb_total.nb_reqs_5s   +=st_sb_cur->nb_reqs_5s;
	    st_sb_total.nb_reqs_10s  +=st_sb_cur->nb_reqs_10s;
	    st_sb_total.nb_reqs_15s  +=st_sb_cur->nb_reqs_15s;
	    st_sb_total.nb_reqs_20s  +=st_sb_cur->nb_reqs_20s;
	    st_sb_total.nb_reqs_30s  +=st_sb_cur->nb_reqs_30s;
	    st_sb_total.nb_reqs_xs   +=st_sb_cur->nb_reqs_xs;

	    #if __APACHE24__
              ap_copy_scoreboard_worker(&ws_record_st,i, j);
	    #else
	      ws_record = ap_get_scoreboard_worker(i, j);
            #endif
            res = ws_record->status;
            stat_buffer[indx] = status_text_flags[res];

	    if (res != SERVER_DEAD && res != SERVER_READY  && st_sb_cur->avg>0)
	    {
		  /* average response time */
		  st_sb_total.avg+=st_sb_cur->avg;
		  nb_avg++;
	    }

	    if (res != SERVER_DEAD && st_sb_cur->percentil>0) 
	    {
	      /* percentil response time */
	      percentil_array[nb_percentil]=st_sb_cur->percentil;
	      nb_percentil++;
	    }

            if (!ps_record->quiescing && ps_record->pid) 
	    {
                if (res == SERVER_READY
                    && ps_record->generation == ap_my_generation)
                    ready++;
                else if (res != SERVER_DEAD &&
                         res != SERVER_STARTING &&
                         res != SERVER_IDLE_KILL)
                    busy++;
            }

            /* XXX what about the counters for quiescing/seg faulted
             * processes?  should they be counted or not?  GLA
             */
	    lres = ws_record->access_count;
	    bytes = ws_record->bytes_served;

	    if (lres != 0 || (res != SERVER_READY && res != SERVER_DEAD)) 
	    {

#ifdef HAVE_TIMES
		tmp_tu = ws_record->times.tms_utime;
		tmp_ts = ws_record->times.tms_stime;
		tmp_tcu = ws_record->times.tms_cutime;
		tmp_tcs = ws_record->times.tms_cstime;

		if (times_per_thread) 
		{
		    proc_tu += tmp_tu;
		    proc_ts += tmp_ts;
		    proc_tcu += tmp_tcu;
		    proc_tcs += tmp_tcs;
		} else {
		    if (tmp_tu > proc_tu ||
			tmp_ts > proc_ts ||
			tmp_tcu > proc_tcu ||
			tmp_tcs > proc_tcs) 
		    {
			proc_tu = tmp_tu;
			proc_ts = tmp_ts;
			proc_tcu = tmp_tcu;
			proc_tcs = tmp_tcs;
		    }
		}
#endif /* HAVE_TIMES */

		count += lres;
		bcount += bytes;

		if (bcount >= KBYTE) 
		{
		    kbcount += (bcount >> 10);
		    bcount = bcount & 0x3ff;
		}
	    }
        }
#ifdef HAVE_TIMES
        tu += proc_tu;
        ts += proc_ts;
        tcu += proc_tcu;
        tcs += proc_tcs;
#endif
        pid_buffer[i] = ps_record->pid;
    }

    /* calculate total average response time */
    if (nb_avg>0) st_sb_total.avg/=nb_avg;

    /* sort the percentil array (insertion sort algo) */
    for(i=1;i<nb_percentil;i++)
    {
      apr_time_t x=percentil_array[i];
      j=i;
      while(j>0&&percentil_array[j-1]>x)
      {
	percentil_array[j]=percentil_array[j-1];
	j=j-1;
      }
      percentil_array[j]=x;
    }
    /* get 90% centil */
    percentilpos=(unsigned long)((double)(nb_percentil)*0.9);
    st_sb_total.percentil=percentil_array[percentilpos];

    /* up_time in seconds */
    up_time = (apr_uint32_t) apr_time_sec(nowtime -
                               ap_scoreboard_image->global->restart_time);

    ap_mpm_query(AP_MPMQ_MAX_DAEMON_USED, &max_daemons);

    /* api rest */
    if (r->args) 
    {
       if (strcasecmp(r->args,"ModuleVersion")==0)
       {
         ap_rvputs(r, MOD_VERSION, "\n", NULL);
         return 0;
       } 
       else if (strcasecmp(r->args,"ApacheServerRoot")==0)
       {
         ap_rvputs(r, ap_server_root, "\n", NULL);
         return 0;
       } 
       else if (strcasecmp(r->args,"ApacheServerDocumentRoot")==0)
       {
         ap_rvputs(r, ap_document_root(r), "\n", NULL);
         return 0;
       }
       else if (strcasecmp(r->args,"ApacheServerConfigFile")==0)
       {
         ap_rvputs(r, ap_conftree->filename, "\n", NULL);
         return 0;
       }
       else if (strcasecmp(r->args,"ApacheServerName")==0)
       {
         ap_rvputs(r, ap_get_server_name(r), "\n", NULL);
         return 0;
       }
       else if (strcasecmp(r->args,"ApacheServerPort")==0)
       {
         ap_rprintf(r, "%d\n", ap_get_server_port(r));
         return 0;
       }
       else if (strcasecmp(r->args,"ApacheListen/count")==0)
       {
	 lr_count=0;
	 for (lr = ap_listeners; lr != NULL; lr = lr->next) lr_count++;
	 ap_rprintf(r, "%d\n", lr_count);
         return 0;
       }
       else if (strncasecmp(r->args,"ApacheListen/",13)==0)
       {
	 int args_index_len=strlen(r->args+13);
	 int listener_index=0;
	 if (args_index_len>0) {
	   listener_index=apr_atoi64(r->args+13);
	   if (listener_index>=0) 
	   {
	     lr_count=0;
	     for (lr = ap_listeners; lr != NULL; lr = lr->next) 
	     {
		if (lr->active&&lr_count==listener_index) 
		{
                  if (lr->bind_addr->hostname)
		  ap_rprintf(r, "%s://%s:%d\n",
		       lr->protocol,
		       lr->bind_addr->hostname,
		       lr->bind_addr->port);
                  else
		  ap_rprintf(r, "%s://0.0.0.0:%d\n",
		       lr->protocol,
		       lr->bind_addr->port);
		}
		lr_count++;
	     }
	   }
	 }
	 if (lr_count<=listener_index) ap_rprintf(r,"Bad Index!\n");
         return 0;
       }
       else if (strcasecmp(r->args,"ApacheServerVersion")==0)
       {
         ap_rvputs(r, AP_SERVER_BASEVERSION " (" PLATFORM ")", "\n", NULL);
         return 0;
       }
       else if (strcasecmp(r->args,"ApacheServerBuilt")==0)
       {
         ap_rvputs(r, ap_get_server_built(), "\n", NULL);
         return 0;
       }
       else if (strcasecmp(r->args,"ApacheAprVersion")==0)
       {
         ap_rvputs(r, apr_version_string(), "\n", NULL);
         return 0;
       }
       else if (strcasecmp(r->args,"ApacheAprBuildVersion")==0)
       {
         ap_rvputs(r, APR_VERSION_STRING, "\n", NULL);
         return 0;
       }
       else if (strcasecmp(r->args,"ApacheApuVersion")==0)
       {
         ap_rvputs(r, apu_version_string(), "\n", NULL);
         return 0;
       }
       else if (strcasecmp(r->args,"ApacheApuBuildVersion")==0)
       {
         ap_rvputs(r, APU_VERSION_STRING, "\n", NULL);
         return 0;
       }
       else if (strcasecmp(r->args,"ApacheServerMPM")==0)
       {
         ap_rprintf(r, "%s\n", ap_show_mpm());
         return 0;
       }
       else if (strcasecmp(r->args,"ApacheThreaded")==0)
       {
         ap_rprintf(r, "%s\n",threaded ? "yes" : "no");
         return 0;
       }
       else if (strcasecmp(r->args,"ApacheForked")==0)
       {
         ap_rprintf(r, "%s\n",forked ? "yes" : "no");
         return 0;
       }
       else if (strcasecmp(r->args,"ApacheServerArchitecture")==0)
       {
         ap_rprintf(r, "%ld\n", 8 * (long) sizeof(void *));
         return 0;
       }
       else if (strcasecmp(r->args,"ApacheTimeout")==0)
       {
         ap_rprintf(r, "%d\n", (int) (apr_time_sec(r->server->timeout)));
         return 0;
       }
       else if (strcasecmp(r->args,"ApacheKeepAliveTimeout")==0)
       {
         ap_rprintf(r, "%d\n", (int) (apr_time_sec(r->server->keep_alive_timeout)));
         return 0;
       }
       else if (strcasecmp(r->args,"ApacheParentServerGeneration")==0)
       {
         ap_rprintf(r, "%d\n", (int)ap_my_generation);
         return 0;
       }
       else if (strcasecmp(r->args,"ApacheCurrentTime")==0)
       {
         ap_rprintf(r, "%"APR_TIME_T_FMT"\n", nowtime);
         return 0;
       }
       else if (strcasecmp(r->args,"ApacheThreadLimit")==0)
       {
         ap_rprintf(r, "%u\n", thread_limit);
         return 0;
       }
       else if (strcasecmp(r->args,"ApacheServerLimit")==0)
       {
         ap_rprintf(r, "%u\n", server_limit);
         return 0;
       }
       else if (strcasecmp(r->args,"ApacheRestartTime")==0)
       {
         ap_rprintf(r, "%"APR_TIME_T_FMT"\n", ap_scoreboard_image->global->restart_time);
         return 0;
       }
       else if (strcasecmp(r->args,"ApacheServerUptime")==0)
       {
         ap_rprintf(r, "%ld\n", (long) (up_time));
         return 0;
       }
       else if (strcasecmp(r->args,"ApacheTotalAccesses")==0)
       {
	 ap_rprintf(r, "%" APR_UINT64_T_FMT "\n", count);
         return 0;
       }
       else if (strcasecmp(r->args,"ApacheTotalKBytes")==0)
       {
	 ap_rprintf(r, "%" APR_UINT64_T_FMT "\n", kbcount);
         return 0;
       }
       else if (strcasecmp(r->args,"ApacheMaxWorker")==0)
       {
	 ap_rprintf(r, "%d\n",max_daemons);
         return 0;
       }
       else if (strcasecmp(r->args,"ApacheBusyWorkers")==0)
       {
	 ap_rprintf(r, "%d\n", busy);
         return 0;
       }
       else if (strcasecmp(r->args,"ApacheIdleWorkers")==0)
       {
	 ap_rprintf(r, "%d\n", ready);
         return 0;
       }
       /* number of request per status code */
       else if (strcasecmp(r->args,"Apache_NB_Reqs_xxx")==0)
       {
	  ap_rprintf(r, "%"APR_UINT64_T_FMT"\n", st_sb_total.nb_reqs_xxx);
	  return 0;
       }
       else if (strcasecmp(r->args,"Apache_NB_Reqs_10x")==0)
       {
	  ap_rprintf(r, "%"APR_UINT64_T_FMT"\n", st_sb_total.nb_reqs_10x);
	  return 0;
       }
       else if (strcasecmp(r->args,"Apache_NB_Reqs_200")==0)
       {
	  ap_rprintf(r, "%"APR_UINT64_T_FMT"\n", st_sb_total.nb_reqs_200);
	  return 0;
       }
       else if (strcasecmp(r->args,"Apache_NB_Reqs_20x")==0)
       {
	  ap_rprintf(r, "%"APR_UINT64_T_FMT"\n", st_sb_total.nb_reqs_20x);
	  return 0;
       }
       else if (strcasecmp(r->args,"Apache_NB_Reqs_301")==0)
       {
	  ap_rprintf(r, "%"APR_UINT64_T_FMT"\n", st_sb_total.nb_reqs_301);
	  return 0;
       }
       else if (strcasecmp(r->args,"Apache_NB_Reqs_302")==0)
       {
	  ap_rprintf(r, "%"APR_UINT64_T_FMT"\n", st_sb_total.nb_reqs_302);
	  return 0;
       }
       else if (strcasecmp(r->args,"Apache_NB_Reqs_304")==0)
       {
	  ap_rprintf(r, "%"APR_UINT64_T_FMT"\n", st_sb_total.nb_reqs_304);
	  return 0;
       }
       else if (strcasecmp(r->args,"Apache_NB_Reqs_30x")==0)
       {
	  ap_rprintf(r, "%"APR_UINT64_T_FMT"\n", st_sb_total.nb_reqs_30x);
	  return 0;
       }
       else if (strcasecmp(r->args,"Apache_NB_Reqs_404")==0)
       {
	  ap_rprintf(r, "%"APR_UINT64_T_FMT"\n", st_sb_total.nb_reqs_404);
	  return 0;
       }
       else if (strcasecmp(r->args,"Apache_NB_Reqs_40x")==0)
       {
	  ap_rprintf(r, "%"APR_UINT64_T_FMT"\n", st_sb_total.nb_reqs_40x);
	  return 0;
       }
       else if (strcasecmp(r->args,"Apache_NB_Reqs_50x")==0)
       {
	  ap_rprintf(r, "%"APR_UINT64_T_FMT"\n", st_sb_total.nb_reqs_50x);
	  return 0;
       }

       /* number of bytes per status code */
       else if (strcasecmp(r->args,"Apache_NB_Bytes_xxx")==0)
       {
	  ap_rprintf(r, "%" APR_UINT64_T_FMT "\n", st_sb_total.nb_bytes_xxx);
	  return 0;
       }
       else if (strcasecmp(r->args,"Apache_NB_Bytes_10x")==0)
       {
	  ap_rprintf(r, "%" APR_UINT64_T_FMT "\n", st_sb_total.nb_bytes_10x);
	  return 0;
       }
       else if (strcasecmp(r->args,"Apache_NB_Bytes_200")==0)
       {
	  ap_rprintf(r, "%" APR_UINT64_T_FMT "\n", st_sb_total.nb_bytes_200);
	  return 0;
       }
       else if (strcasecmp(r->args,"Apache_NB_Bytes_20x")==0)
       {
	  ap_rprintf(r, "%" APR_UINT64_T_FMT "\n", st_sb_total.nb_bytes_20x);
	  return 0;
       }
       else if (strcasecmp(r->args,"Apache_NB_Bytes_301")==0)
       {
	  ap_rprintf(r, "%" APR_UINT64_T_FMT "\n", st_sb_total.nb_bytes_301);
	  return 0;
       }
       else if (strcasecmp(r->args,"Apache_NB_Bytes_302")==0)
       {
	  ap_rprintf(r, "%" APR_UINT64_T_FMT "\n", st_sb_total.nb_bytes_302);
	  return 0;
       }
       else if (strcasecmp(r->args,"Apache_NB_Bytes_304")==0)
       {
	  ap_rprintf(r, "%" APR_UINT64_T_FMT "\n", st_sb_total.nb_bytes_304);
	  return 0;
       }
       else if (strcasecmp(r->args,"Apache_NB_Bytes_30x")==0)
       {
	  ap_rprintf(r, "%" APR_UINT64_T_FMT "\n", st_sb_total.nb_bytes_30x);
	  return 0;
       }
       else if (strcasecmp(r->args,"Apache_NB_Bytes_404")==0)
       {
	  ap_rprintf(r, "%" APR_UINT64_T_FMT "\n", st_sb_total.nb_bytes_404);
	  return 0;
       }
       else if (strcasecmp(r->args,"Apache_NB_Bytes_40x")==0)
       {
	  ap_rprintf(r, "%" APR_UINT64_T_FMT "\n", st_sb_total.nb_bytes_40x);
	  return 0;
       }
       else if (strcasecmp(r->args,"Apache_NB_Bytes_50x")==0)
       {
	  ap_rprintf(r, "%" APR_UINT64_T_FMT "\n", st_sb_total.nb_bytes_50x);
	  return 0;
       }

       /* number of request per response times range */
       else if (strcasecmp(r->args,"Apache_NB_Reqs_50ms")==0)
       {
	  ap_rprintf(r, "%"APR_TIME_T_FMT"\n", st_sb_total.nb_reqs_50ms);
	  return 0;
       }
       else if (strcasecmp(r->args,"Apache_NB_Reqs_100ms")==0)
       {
	  ap_rprintf(r, "%"APR_TIME_T_FMT"\n", st_sb_total.nb_reqs_100ms);
	  return 0;
       }
       else if (strcasecmp(r->args,"Apache_NB_Reqs_300ms")==0)
       {
	  ap_rprintf(r, "%"APR_TIME_T_FMT"\n", st_sb_total.nb_reqs_300ms);
	  return 0;
       }
       else if (strcasecmp(r->args,"Apache_NB_Reqs_500ms")==0)
       {
	  ap_rprintf(r, "%"APR_TIME_T_FMT"\n", st_sb_total.nb_reqs_500ms);
	  return 0;
       }
       else if (strcasecmp(r->args,"Apache_NB_Reqs_1s")==0)
       {
	  ap_rprintf(r, "%"APR_TIME_T_FMT"\n", st_sb_total.nb_reqs_1s);
	  return 0;
       }
       else if (strcasecmp(r->args,"Apache_NB_Reqs_1_5s")==0)
       {
	  ap_rprintf(r, "%"APR_TIME_T_FMT"\n", st_sb_total.nb_reqs_1_5s);
	  return 0;
       }
       else if (strcasecmp(r->args,"Apache_NB_Reqs_2s")==0)
       {
	  ap_rprintf(r, "%"APR_TIME_T_FMT"\n", st_sb_total.nb_reqs_2s);
	  return 0;
       }
       else if (strcasecmp(r->args,"Apache_NB_Reqs_5s")==0)
       {
	  ap_rprintf(r, "%"APR_TIME_T_FMT"\n", st_sb_total.nb_reqs_5s);
	  return 0;
       }
       else if (strcasecmp(r->args,"Apache_NB_Reqs_10s")==0)
       {
	  ap_rprintf(r, "%"APR_TIME_T_FMT"\n", st_sb_total.nb_reqs_10s);
	  return 0;
       }
       else if (strcasecmp(r->args,"Apache_NB_Reqs_15s")==0)
       {
	  ap_rprintf(r, "%"APR_TIME_T_FMT"\n", st_sb_total.nb_reqs_15s);
	  return 0;
       }
       else if (strcasecmp(r->args,"Apache_NB_Reqs_20s")==0)
       {
	  ap_rprintf(r, "%"APR_TIME_T_FMT"\n", st_sb_total.nb_reqs_20s);
	  return 0;
       }
       else if (strcasecmp(r->args,"Apache_NB_Reqs_30s")==0)
       {
	  ap_rprintf(r, "%"APR_TIME_T_FMT"\n", st_sb_total.nb_reqs_30s);
	  return 0;
       }
       else if (strcasecmp(r->args,"Apache_NB_Reqs_xs")==0)
       {
	  ap_rprintf(r, "%"APR_TIME_T_FMT"\n", st_sb_total.nb_reqs_xs);
	  return 0;
       }
       else if (strcasecmp(r->args,"Apache_Avg_ResponseTime")==0)
       {
	  ap_rprintf(r, "%"APR_TIME_T_FMT"\n", st_sb_total.avg);
	  return 0;
       }
       else if (strcasecmp(r->args,"Apache_90Percentil_ResponseTime")==0)
       {
	  ap_rprintf(r, "%"APR_TIME_T_FMT"\n", st_sb_total.percentil);
	  return 0;
       }
#ifdef HAVE_TIMES
       else if (strcasecmp(r->args,"ApacheCPUUsage.User")==0)
       {
	 ap_rprintf(r, "%g\n", tu / tick);
         return 0;
       }
       else if (strcasecmp(r->args,"ApacheCPUUsage.System")==0)
       {
	 ap_rprintf(r, "%g\n", ts / tick);
         return 0;
       }
       else if (strcasecmp(r->args,"ApacheChildCPUUsage.User")==0)
       {
	 ap_rprintf(r, "%g\n", tcu / tick);
         return 0;
       }
       else if (strcasecmp(r->args,"ApacheChildCPUUsage.System")==0)
       {
	 ap_rprintf(r, "%g\n", tcs / tick);
         return 0;
       }
       else if (strcasecmp(r->args,"ApacheCPULoad")==0)
       {
	 if (ts || tu || tcu || tcs)
	     ap_rprintf(r, "%g\n",
			(tu + ts + tcu + tcs) / tick / up_time * 100.);
         else
	     ap_rprintf(r, "na\n");
         return 0;
       }
#endif
       else
       {
	 ap_rprintf(r, "Unknown Attribut!\n");
         return 0;
       }
    }

    /* without argument print all */

    ap_rvputs(r, "ModuleVersion: ", MOD_VERSION, "\n", NULL);
    ap_rvputs(r, "ApacheServerRoot: ", 		ap_server_root, "\n", NULL);
    ap_rvputs(r, "ApacheServerDocumentRoot: ", 		ap_document_root(r), "\n", NULL);
    ap_rvputs(r, "ApacheServerConfigFile: ", 		ap_conftree->filename, "\n", NULL);
    ap_rvputs(r, "ApacheServerName: ", 		ap_get_server_name(r), "\n", NULL);
    ap_rprintf(r, "ApacheServerPort: %d\n", 		ap_get_server_port(r));

    lr_count=0;
    for (lr = ap_listeners; lr != NULL; lr = lr->next) 
    {
       if (lr->active) 
       {
                  if (lr->bind_addr->hostname)
	 ap_rprintf(r, "ApacheListen[%d]: %s://%s:%d\n",
	      lr_count,
	      lr->protocol,
	      lr->bind_addr->hostname,
	      lr->bind_addr->port);
                  else
	 ap_rprintf(r, "ApacheListen[%d]: %s://%s:%d\n",
	      lr_count,
	      lr->protocol,
	      "0.0.0.0",
	      lr->bind_addr->port);
       }
       lr_count++;
    }

    ap_rvputs(r, "ApacheServerVersion: ", 	AP_SERVER_BASEVERSION " (" PLATFORM ")", "\n", NULL);
    ap_rvputs(r, "ApacheServerBuilt: ", 	ap_get_server_built(), "\n", NULL);
    ap_rvputs(r, "ApacheAprVersion: ", 	apr_version_string(), "\n", NULL);
    ap_rvputs(r, "ApacheAprBuildVersion: ", 	APR_VERSION_STRING, "\n", NULL);
    ap_rvputs(r, "ApacheApuVersion: ", 	apu_version_string(), "\n", NULL);
    ap_rvputs(r, "ApacheApuBuildVersion: ", 	APU_VERSION_STRING, "\n", NULL);
    ap_rprintf(r, "ApacheServerMPM: %s\n", ap_show_mpm());
    ap_rprintf(r, "ApacheThreaded: %s\n",threaded ? "yes" : "no");
    ap_rprintf(r, "ApacheForked: %s\n",forked ? "yes" : "no");
    ap_rprintf(r, "ApacheServerArchitecture: %ld-bit\n",8 * (long) sizeof(void *));
    ap_rprintf(r, "ApacheTimeout: %d\n", (int) (apr_time_sec(r->server->timeout)));
    ap_rprintf(r, "ApacheKeepAliveTimeout: %d\n", (int) (apr_time_sec(r->server->keep_alive_timeout)));
    ap_rprintf(r, "ApacheParentServerGeneration: %d\n", (int)ap_my_generation);
    ap_rprintf(r, "ApacheCurrentTime: %"APR_TIME_T_FMT"\n", nowtime);
    ap_rprintf(r, "ApacheRestartTime: %"APR_TIME_T_FMT"\n", ap_scoreboard_image->global->restart_time);
    ap_rprintf(r, "ApacheServerUptime: %ld\n", (long) (up_time));

    /* number of request per status code */
    ap_rprintf(r, "Apache_NB_Reqs_xxx: %"APR_UINT64_T_FMT"\n", st_sb_total.nb_reqs_xxx);
    ap_rprintf(r, "Apache_NB_Reqs_10x: %"APR_UINT64_T_FMT"\n", st_sb_total.nb_reqs_10x);
    ap_rprintf(r, "Apache_NB_Reqs_200: %"APR_UINT64_T_FMT"\n", st_sb_total.nb_reqs_200);
    ap_rprintf(r, "Apache_NB_Reqs_20x: %"APR_UINT64_T_FMT"\n", st_sb_total.nb_reqs_20x);
    ap_rprintf(r, "Apache_NB_Reqs_301: %"APR_UINT64_T_FMT"\n", st_sb_total.nb_reqs_301);
    ap_rprintf(r, "Apache_NB_Reqs_302: %"APR_UINT64_T_FMT"\n", st_sb_total.nb_reqs_302);
    ap_rprintf(r, "Apache_NB_Reqs_304: %"APR_UINT64_T_FMT"\n", st_sb_total.nb_reqs_304);
    ap_rprintf(r, "Apache_NB_Reqs_30x: %"APR_UINT64_T_FMT"\n", st_sb_total.nb_reqs_30x);
    ap_rprintf(r, "Apache_NB_Reqs_404: %"APR_UINT64_T_FMT"\n", st_sb_total.nb_reqs_404);
    ap_rprintf(r, "Apache_NB_Reqs_40x: %"APR_UINT64_T_FMT"\n", st_sb_total.nb_reqs_40x);
    ap_rprintf(r, "Apache_NB_Reqs_50x: %"APR_UINT64_T_FMT"\n", st_sb_total.nb_reqs_50x);

    /* number of bytes per status code */
    ap_rprintf(r, "Apache_NB_Bytes_xxx: %" APR_UINT64_T_FMT "\n", st_sb_total.nb_bytes_xxx);
    ap_rprintf(r, "Apache_NB_Bytes_10x: %" APR_UINT64_T_FMT "\n", st_sb_total.nb_bytes_10x);
    ap_rprintf(r, "Apache_NB_Bytes_200: %" APR_UINT64_T_FMT "\n", st_sb_total.nb_bytes_200);
    ap_rprintf(r, "Apache_NB_Bytes_20x: %" APR_UINT64_T_FMT "\n", st_sb_total.nb_bytes_20x);
    ap_rprintf(r, "Apache_NB_Bytes_301: %" APR_UINT64_T_FMT "\n", st_sb_total.nb_bytes_301);
    ap_rprintf(r, "Apache_NB_Bytes_302: %" APR_UINT64_T_FMT "\n", st_sb_total.nb_bytes_302);
    ap_rprintf(r, "Apache_NB_Bytes_304: %" APR_UINT64_T_FMT "\n", st_sb_total.nb_bytes_304);
    ap_rprintf(r, "Apache_NB_Bytes_30x: %" APR_UINT64_T_FMT "\n", st_sb_total.nb_bytes_30x);
    ap_rprintf(r, "Apache_NB_Bytes_404: %" APR_UINT64_T_FMT "\n", st_sb_total.nb_bytes_404);
    ap_rprintf(r, "Apache_NB_Bytes_40x: %" APR_UINT64_T_FMT "\n", st_sb_total.nb_bytes_40x);
    ap_rprintf(r, "Apache_NB_Bytes_50x: %" APR_UINT64_T_FMT "\n", st_sb_total.nb_bytes_50x);

    /* number of request per response times range */
    ap_rprintf(r, "Apache_NB_Reqs_50ms: %"APR_TIME_T_FMT"\n", st_sb_total.nb_reqs_50ms);
    ap_rprintf(r, "Apache_NB_Reqs_100ms: %"APR_TIME_T_FMT"\n", st_sb_total.nb_reqs_100ms);
    ap_rprintf(r, "Apache_NB_Reqs_300ms: %"APR_TIME_T_FMT"\n", st_sb_total.nb_reqs_300ms);
    ap_rprintf(r, "Apache_NB_Reqs_500ms: %"APR_TIME_T_FMT"\n", st_sb_total.nb_reqs_500ms);
    ap_rprintf(r, "Apache_NB_Reqs_1s: %"APR_TIME_T_FMT"\n", st_sb_total.nb_reqs_1s);
    ap_rprintf(r, "Apache_NB_Reqs_1_5s: %"APR_TIME_T_FMT"\n", st_sb_total.nb_reqs_1_5s);
    ap_rprintf(r, "Apache_NB_Reqs_2s: %"APR_TIME_T_FMT"\n", st_sb_total.nb_reqs_2s);
    ap_rprintf(r, "Apache_NB_Reqs_5s: %"APR_TIME_T_FMT"\n", st_sb_total.nb_reqs_5s);
    ap_rprintf(r, "Apache_NB_Reqs_10s: %"APR_TIME_T_FMT"\n", st_sb_total.nb_reqs_10s);
    ap_rprintf(r, "Apache_NB_Reqs_15s: %"APR_TIME_T_FMT"\n", st_sb_total.nb_reqs_15s);
    ap_rprintf(r, "Apache_NB_Reqs_20s: %"APR_TIME_T_FMT"\n", st_sb_total.nb_reqs_20s);
    ap_rprintf(r, "Apache_NB_Reqs_30s: %"APR_TIME_T_FMT"\n", st_sb_total.nb_reqs_30s);
    ap_rprintf(r, "Apache_NB_Reqs_xs: %"APR_TIME_T_FMT"\n", st_sb_total.nb_reqs_xs);

    ap_rprintf(r, "Apache_Avg_ResponseTime: %"APR_TIME_T_FMT"\n", st_sb_total.avg);
    ap_rprintf(r, "Apache_Percentil_Array:%"APR_TIME_T_FMT"", percentil_array[0]);
    for(i=1;i<nb_percentil;i++)
    ap_rprintf(r, ",%"APR_TIME_T_FMT"", percentil_array[i]);
    ap_rprintf(r, "\n");
    
    ap_rprintf(r, "Apache_90Percentil_ResponseTime: %"APR_TIME_T_FMT"\n", st_sb_total.percentil);

    ap_rprintf(r, "ApacheTotalAccesses: %"APR_UINT64_T_FMT"\n", count);
    ap_rprintf(r, "ApacheTotalKBytes: %" APR_UINT64_T_FMT "\n", kbcount);

    ap_rprintf(r, "ApacheThreadLimit: %u\n", thread_limit);
    ap_rprintf(r, "ApacheServerLimit: %u\n", server_limit);
    ap_rprintf(r, "ApacheMaxWorker: %d\n",max_daemons);
    ap_rprintf(r, "ApacheBusyWorkers: %d\n", busy);
    ap_rprintf(r, "ApacheIdleWorkers: %d\n", ready);

#ifdef HAVE_TIMES
    ap_rprintf(r, "ApacheCPUUsage.User: %g\n", tu / tick);
    ap_rprintf(r, "ApacheCPUUsage.System: %g\n", ts / tick);
    ap_rprintf(r, "ApacheChildCPUUsage.User: %g\n", tcu / tick);
    ap_rprintf(r, "ApacheChildCPUUsage.System: %g\n", tcs / tick);

    if (ts || tu || tcu || tcs)
	ap_rprintf(r, "ApacheCPULoad: %g\n",
		   (tu + ts + tcu + tcs) / tick / up_time * 100.);
#endif

    /* send the scoreboard 'table' out */
    /*
     ap_rputs("#ApacheWorker[ServIndx-ServGen]: Pid;Acc;Status;CPU;SrvTime;ReqTime;Conn;Client;Request\n",r);
     */

    for (i = 0; i < server_limit; ++i) 
    {
	for (j = 0; j < thread_limit; ++j) 
	{
            int indx = (i * thread_limit) + j;

	    /* get the current status text scoreboard */
	    st_sb_cur=&st_sb[indx];

	    #if __APACHE24__
              ap_copy_scoreboard_worker(&ws_record_st,i, j);
	    #else
	      ws_record = ap_get_scoreboard_worker(i, j);
            #endif

	    if (ws_record->access_count == 0 &&
		(ws_record->status == SERVER_READY ||
		 ws_record->status == SERVER_DEAD)) 
		     continue;

	    ps_record = ap_get_scoreboard_process(i);

	    if (ws_record->start_time == 0L)
		req_time = 0L;
	    else
		req_time = (long)
		    ((ws_record->stop_time -
		      ws_record->start_time) / 1000);
	    if (req_time < 0L)
		req_time = 0L;

	    lres = ws_record->access_count;
	    my_lres = ws_record->my_access_count;
	    conn_lres = ws_record->conn_count;
	    bytes = ws_record->bytes_served;
	    my_bytes = ws_record->my_bytes_served;
	    conn_bytes = ws_record->conn_bytes;
	    if (ws_record->pid) 
	    { /* MPM sets per-worker pid and generation */
		worker_pid = ws_record->pid;
		worker_generation = ws_record->generation;
	    } else {
		worker_pid = ps_record->pid;
		worker_generation = ps_record->generation;
	    }

	    if (ws_record->status == SERVER_DEAD)
		ap_rprintf(r, "ApacheWorker[%d-%d-%d]: -;%d|%" APR_UINT64_T_FMT "|%" APR_UINT64_T_FMT ";",
			   i, j, (int)worker_generation,
			   (int)conn_lres, my_lres, lres);
	    else
		ap_rprintf(r, "ApacheWorker[%d-%d-%d]: %"
			   APR_PID_T_FMT ";%d|%" APR_UINT64_T_FMT "|%" APR_UINT64_T_FMT ";",
			   i, j, (int) worker_generation,
			   worker_pid,
			   (int)conn_lres, my_lres, lres);

	    switch (ws_record->status) 
	    {
	      case SERVER_READY:
		  ap_rputs("Ready", r);
		  break;
	      case SERVER_STARTING:
		  ap_rputs("Starting", r);
		  break;
	      case SERVER_BUSY_READ:
		  ap_rputs("Read", r);
		  break;
	      case SERVER_BUSY_WRITE:
		  ap_rputs("Write", r);
		  break;
	      case SERVER_BUSY_KEEPALIVE:
		  ap_rputs("Keepalive", r);
		  break;
	      case SERVER_BUSY_LOG:
		  ap_rputs("Logging>", r);
		  break;
	      case SERVER_BUSY_DNS:
		  ap_rputs("DNS lookup", r);
		  break;
	      case SERVER_CLOSING:
		  ap_rputs("Closing", r);
		  break;
	      case SERVER_DEAD:
		  ap_rputs("Dead", r);
		  break;
	      case SERVER_GRACEFUL:
		  ap_rputs("Graceful", r);
		  break;
	      case SERVER_IDLE_KILL:
		  ap_rputs("Dying", r);
		  break;
	      default:
		  ap_rputs("?STATE?", r);
		  break;
	    }

#ifdef HAVE_TIMES
	    ap_rprintf(r, ";%g|%g|%g|%g",
		       ws_record->times.tms_utime / tick,
		       ws_record->times.tms_stime / tick,
		       ws_record->times.tms_cutime / tick,
		       ws_record->times.tms_cstime / tick);
#else
	    ap_rputs( ";-|-|-|-", r);
#endif

	    ap_rprintf(r, ";%ld;%"APR_TIME_T_FMT";%"APR_TIME_T_FMT";%"APR_TIME_T_FMT";%ld",
		       (long) apr_time_sec(nowtime -
					  ws_record->last_used),
		       st_sb_cur->last,
		       st_sb_cur->avg,
		       st_sb_cur->percentil,
		       req_time);

	    ap_rprintf(r, ";%"APR_UINT64_T_FMT"|%"APR_UINT64_T_FMT"|%" APR_UINT64_T_FMT ,  
			  conn_bytes, 
			  my_bytes , 
			  bytes);

	    ap_rprintf(r,
		       ";%s|%s|%s\n",
		       ap_escape_html(r->pool,
				      ws_record->client),
		       ap_escape_html(r->pool,
				      ap_escape_logitem(r->pool,
							ws_record->request)),
		       ap_escape_html(r->pool,
				      ws_record->vhost));
	} /* for (j...) */
    } /* for (i...) */

    return 0;
}

void status_text_log_listen(apr_pool_t *p, server_rec*s, const char *filename)
{
    int lr_count=0;
    ap_listen_rec *lr=NULL;
    apr_file_t *listen_file = NULL;
    apr_status_t rv;
    const char *fname;

    if (!filename) {
        return;
    }

    fname = ap_server_root_relative(p, filename);
    if (!fname) {
        ap_log_error(APLOG_MARK, APLOG_STARTUP|APLOG_CRIT, APR_EBADPATH,
                     NULL, "Invalid PID file path %s, ignoring.", filename);
        return;
    }

    core_server_config *conf;
    conf = (core_server_config *)ap_get_module_config(s->module_config,
							  &core_module);

    if ((rv = apr_file_open(&listen_file, fname,
                            APR_WRITE | APR_CREATE | APR_TRUNCATE,
                            APR_UREAD | APR_UWRITE | APR_GREAD | APR_WREAD, p))
        != APR_SUCCESS) {
        ap_log_error(APLOG_MARK, APLOG_ERR, rv, NULL,
                     "could not create %s", fname);
        ap_log_error(APLOG_MARK, APLOG_ERR, 0, NULL,
                     "%s: could not log listen information to file %s",
                     ap_server_argv0, fname);
        exit(1);
    }
    apr_file_printf(listen_file, "ApacheServerRoot: %s\n",ap_server_root);
    apr_file_printf(listen_file, "ApacheServerDocumentRoot: %s\n",conf->ap_document_root);

    lr_count=0;
    for (lr = ap_listeners; lr != NULL; lr = lr->next) 
    {
       lr_count++;
       if (lr->active) 
       {
         if (lr->bind_addr->hostname)
	 apr_file_printf(listen_file, "ApacheListen[%d]: %s://%s:%d\n",
	      lr_count,
	      lr->protocol,
	      lr->bind_addr->hostname,
	      lr->bind_addr->port);
         else
	 apr_file_printf(listen_file, "ApacheListen[%d]: %s://%s:%d\n",
	      lr_count,
	      lr->protocol,
	      "0.0.0.0",
	      lr->bind_addr->port);
       }
    }
    apr_file_close(listen_file);
}

static int status_text_init(apr_pool_t *p, apr_pool_t *plog, apr_pool_t *ptemp,
                       server_rec *s)
{
    status_text_flags[SERVER_DEAD] = '.';  /* We don't want to assume these are in */
    status_text_flags[SERVER_READY] = '_'; /* any particular order in scoreboard.h */
    status_text_flags[SERVER_STARTING] = 'S';
    status_text_flags[SERVER_BUSY_READ] = 'R';
    status_text_flags[SERVER_BUSY_WRITE] = 'W';
    status_text_flags[SERVER_BUSY_KEEPALIVE] = 'K';
    status_text_flags[SERVER_BUSY_LOG] = 'L';
    status_text_flags[SERVER_BUSY_DNS] = 'D';
    status_text_flags[SERVER_CLOSING] = 'C';
    status_text_flags[SERVER_GRACEFUL] = 'G';
    status_text_flags[SERVER_IDLE_KILL] = 'I';
    /* TODO: must be configurable */
    status_text_log_listen(p,s,"var/listen.txt");
    /* force extended status activation */
    ap_extended_status=1;
    return OK;
}

static void status_text_child_init(apr_pool_t *p, server_rec *s)
{
#ifdef HAVE_TIMES
    child_pid = getpid();
#endif
}

static apr_status_t status_text_cleanup_scoreboard(void *d)
{
     if (status_text_scoreboard_shm!=NULL) apr_shm_destroy(status_text_scoreboard_shm);
     return APR_SUCCESS;
}

int status_text_create_scoreboard(apr_pool_t *p, ap_scoreboard_e sb_type)
{
    apr_status_t rv;
    apr_pool_t *global_pool;
    //void *sb_shared;
    char *fname = NULL;

    if (sb_type != SB_SHARED) {
	ap_log_error(APLOG_MARK, APLOG_CRIT, 0, NULL,
		     "Only Shared type status_text scoreboard supported");
	return HTTP_INTERNAL_SERVER_ERROR;
    }

    /* initialisze globale vars */
    ap_mpm_query(AP_MPMQ_HARD_LIMIT_THREADS, &thread_limit);
    ap_mpm_query(AP_MPMQ_HARD_LIMIT_DAEMONS, &server_limit);
    ap_mpm_query(AP_MPMQ_IS_THREADED, &threaded);
    ap_mpm_query(AP_MPMQ_IS_FORKED, &forked);

    /* calculate the status text scoreboard size */
    if (threaded) status_text_scoreboard_size = sizeof(status_text_scoreboard_t) * server_limit * thread_limit;
    else status_text_scoreboard_size = sizeof(status_text_scoreboard_t) * server_limit;

    /* We don't want to have to recreate the scoreboard after
     * restarts, so we'll create a global pool and never clean it.
     */
    rv = apr_pool_create(&global_pool, NULL);
    if (rv != APR_SUCCESS) {
        ap_log_error(APLOG_MARK, APLOG_CRIT, rv, NULL,
                     "Fatal error: unable to create global pool "
                     "for use with by the scoreboard");
        return rv;
    }

    /* generate the shm file name */
    status_text_scorebored_name = "logs/scoreboard_runtime";
    fname = ap_server_root_relative(p, status_text_scorebored_name);

    /* try anonymous shared memory before */
    rv = apr_shm_create(&status_text_scoreboard_shm, status_text_scoreboard_size, NULL,
			global_pool); /* anonymous shared memory */
    if ((rv != APR_SUCCESS) && (rv != APR_ENOTIMPL)) {
	ap_log_error(APLOG_MARK, APLOG_CRIT, rv, NULL,
		     "Unable to create or access status_text scoreboard "
		     "(anonymous shared memory failure)");
	return rv;
    }
    /* to do name-based shmem */
    else if (rv == APR_ENOTIMPL) {
	/* The shared memory file must not exist before we create the
	 * segment. */
	apr_shm_remove(fname, p); /* ignore errors */

	rv = apr_shm_create(&status_text_scoreboard_shm, status_text_scoreboard_size, fname, global_pool);
	if (rv != APR_SUCCESS) {
	    ap_log_error(APLOG_MARK, APLOG_CRIT, rv, NULL,
			 "unable to create or access status_text scoreboard \"%s\" "
			 "(name-based shared memory failure)", fname);
	}
    }
    if (rv || !(status_text_scoreboard = apr_shm_baseaddr_get(status_text_scoreboard_shm))) {
	return HTTP_INTERNAL_SERVER_ERROR;
    }

    /* initialize the scoreboard */
    status_text_scoreboard_size = apr_shm_size_get(status_text_scoreboard_shm);
    memset(status_text_scoreboard,0,status_text_scoreboard_size);

    /* register auto cleanup of the shm */
    apr_pool_cleanup_register(p, NULL, status_text_cleanup_scoreboard, apr_pool_cleanup_null);

    return APR_SUCCESS;
}

static void register_hooks(apr_pool_t *p)
{
    //static const char * const aszPre[]={ "mod_status.c",NULL };
    ap_hook_log_transaction(runtime_statistique,NULL,NULL,APR_HOOK_MIDDLE);
    ap_hook_handler(status_text_handler, NULL, NULL, APR_HOOK_MIDDLE);
    //ap_hook_post_config(status_text_init, aszPre, NULL, APR_HOOK_MIDDLE);
    ap_hook_post_config(status_text_init, NULL, NULL, APR_HOOK_MIDDLE);
    ap_hook_child_init(status_text_child_init, NULL, NULL, APR_HOOK_MIDDLE);
    ap_hook_pre_mpm(status_text_create_scoreboard, NULL, NULL, APR_HOOK_MIDDLE);
}

module AP_MODULE_DECLARE_DATA status_text_module =
{
    STANDARD20_MODULE_STUFF,
    NULL,                       /* dir config creater */
    NULL,                       /* dir merger --- default is to override */
    NULL,                       /* server config */
    NULL,                       /* merge server config */
    NULL,         		/* command table */
    register_hooks              /* register_hooks */
};

