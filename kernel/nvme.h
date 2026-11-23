#ifndef RVOS_NVME_H
#define RVOS_NVME_H

#include "types.h"

void nvme_init(void);
int  nvme_read(int index, u32 lba, u32 count, void *buf);
int  nvme_write(int index, u32 lba, u32 count, const void *buf);
int  nvme_flush(int index);

#endif
