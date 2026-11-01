#pragma once
#ifdef LAB_PS4
extern void lab_session_trace(const char *,const char *);
extern void lab_session_trace_request(const char *,const char *);
#else
#define lab_session_trace(a,b) ((void)0)
#define lab_session_trace_request(a,b) ((void)0)
#endif
