#ifndef RVOS_INSTALLER_H
#define RVOS_INSTALLER_H

#include "types.h"
#include "accounts.h"

#define INSTALL_LOG_LINES 8

typedef enum {
    INSTALL_IDLE = 0,
    INSTALL_RUNNING,
    INSTALL_DONE,
    INSTALL_FAILED
} InstallPhase;

typedef struct {
    int phase;
    int target;                 /* ATA device index */
    int boot_dev;
    int step;
    int percent;
    char status[80];
    char log[INSTALL_LOG_LINES][72];
    int log_count;
    char account_user[USER_NAME_MAX + 1];
    char account_pass[USER_PASS_MAX + 1];
    int account_ready;
} Installer;

extern Installer installer;

void installer_init(u32 bios_boot_drive);
int  installer_drive_count(void);
void installer_select(int dev);
int  installer_target(void);
void installer_set_account(const char *user, const char *pass);
int  installer_account_ready(void);
void installer_start(int dev);
void installer_poll(void);
int  installer_installing(void);
void installer_reboot(void);

#endif
