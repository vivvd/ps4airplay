/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "lab.h"
#include <pthread.h>
#include <stdio.h>
#include <signal.h>
#include <string.h>
#include <errno.h>

int lab_airplay_signals_init(void) {
    struct sigaction action;
    memset(&action,0,sizeof(action));
    action.sa_handler=SIG_IGN;
    /* Orbis and host both use 13 for SIGPIPE. Use sigaction with flags=0:
     * OpenOrbis's musl signal() carries incompatible Linux flag values. */
    if(sigaction(13,&action,NULL)!=0)return -errno;
    return 0;
}

int lab_airplay_thread_create(pthread_t *handle,void *(*entry)(void *),void *arg) {
    pthread_attr_t attr;
    int rc=pthread_attr_init(&attr);
    if(rc)return rc;
    rc=pthread_attr_setstacksize(&attr,2*1024*1024);
    if(!rc)rc=pthread_create(handle,&attr,entry,arg);
    pthread_attr_destroy(&attr);
    char status[64];snprintf(status,sizeof(status),"stack=2097152 rc=%d",rc);
    lab_session_trace("worker create",status);
    return rc;
}
