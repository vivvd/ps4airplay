/* SPDX-License-Identifier: GPL-3.0-or-later
 * GoldHEN SDK ABI: GoldHEN/GoldHEN_Plugins_SDK include/GoldHEN.h.
 * ABI layout derived from that MIT-licensed SDK; license in third_party/goldhen.
 * No kernel offsets or firmware-specific memory writes are used here.
 */
#include "lab.h"
#include <errno.h>
#include <string.h>
#include <signal.h>

typedef struct {
    uint32_t cr_uid, cr_ruid, cr_rgid, cr_groups;
    uint64_t cr_paid, cr_caps[2];
    void *cr_prison, *fd_cdir, *fd_jdir, *fd_rdir;
} HenBackup;
_Static_assert(sizeof(HenBackup) == 72, "GoldHEN x86-64 ABI mismatch");
static HenBackup backup;
static int active, attempted, open_result;

#ifdef LAB_HEN_TEST
extern long lab_hen_command(unsigned long command, void *data);
#else
/* SDK uses FreeBSD's indirect syscall 0, with 500 as its first argument.
 * Return negative kernel errno directly, independently of musl's errno ABI.
 */
static long lab_hen_command(unsigned long command, void *data) {
    long result = 0;
    long number = 500;
    unsigned char failed;
    __asm__ volatile("syscall; setc %1"
        : "+a"(result), "=qm"(failed), "+D"(number), "+S"(command), "+d"(data)
        :
        : "rcx", "r8", "r9", "r10", "r11", "cc", "memory");
    return failed ? -result : result;
}
#endif

int lab_hen_open(LabState *s) {
    if (active) return 0;
    if (attempted) return open_result;
    attempted = 1;
    open_result = -1;
    /* Orbis SIGSYS is 12; the OpenOrbis signal.h constant is from Linux.
     * Ignore only while probing, then restore the previous disposition.
     * This runs on the main thread before AirPlay creates worker threads.
     */
    struct sigaction ignored, previous;
    memset(&ignored,0,sizeof(ignored));
    ignored.sa_handler=SIG_IGN;
    /* Avoid signal(): musl's SA_RESTART constant is Linux's, while this
     * sigaction structure is correctly laid out for Orbis in OpenOrbis. */
    if (sigaction(12,&ignored,&previous) != 0) {
        s->hen_probe_rc = -errno;
        lab_log(s,"HEN: cannot guard SDK probe: %d",s->hen_probe_rc);
        return -1;
    }
    long version = lab_hen_command(0, NULL);
    int restore_signal = sigaction(12,&previous,NULL) != 0 ? -errno : 0;
    s->hen_probe_rc = version < 0 ? (int)version : 0;
    /* Hardware 0.08 returned -256 for command 0, matching the magnitude
     * of SDK 1.00. Treat it as a candidate version signature. Recognize exactly
     * +/-0x100 for this read-only command; keep the raw result in probe_rc.
     * Never normalize errors from privilege acquisition/restoration.
     */
    int known_version = version == 0x100 || version == -0x100;
    s->hen_sdk_version = known_version ? 0x100 : 0;
    if (version == -0x100)
        lab_log(s,"HEN: version signature -0x100; checking privilege command separately");
    lab_log(s,"HEN: SDK probe rc=%d version=0x%x signal_restore=%d",
            s->hen_probe_rc,s->hen_sdk_version,restore_signal);
    /* Accept only the ABI whose backup layout is known. Unsupported HENs
     * remain usable for diagnostics; do not issue privilege commands to them.
     */
    if (!known_version || restore_signal != 0) {
        lab_log(s,"HEN: GoldHEN SDK 1.00 required; no privilege request sent");
        return -1;
    }
    memset(&backup,0,sizeof(backup));
    s->hen_acquire_rc = (int)lab_hen_command(2,&backup);
    lab_log(s,"HEN: process jailbreak rc=%d",s->hen_acquire_rc);
    if (s->hen_acquire_rc != 0) return -1;
    active = s->hen_active = 1;
    open_result = 0;
    return 0;
}

int lab_hen_close(LabState *s) {
    if (!active) return 0;
    /* Called after all networking/audio workers have stopped. Keep the backup
     * intact if restoration fails; never report an unsuccessful restore as OK.
     */
    s->hen_restore_rc = (int)lab_hen_command(3,&backup);
    lab_log(s,"HEN: restore process credentials rc=%d",s->hen_restore_rc);
    if (s->hen_restore_rc != 0) return -1;
    active = s->hen_active = 0;
    memset(&backup,0,sizeof(backup));
    open_result = -1; /* closed bridge is intentionally not reopened */
    return 0;
}
