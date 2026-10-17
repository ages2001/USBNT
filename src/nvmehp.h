/* nvmehp.h - NVMe drives behind PCIe hot plug ports (nvmehp.c) */
#ifndef NVMEHP_H
#define NVMEHP_H

void    NvmeHotAttach(UCHAR Bus, UCHAR Dev, UCHAR Fn);
void    NvmeHotService(void);
void    NvmeHotStop(UCHAR First, UCHAR Last);
void    NvmeShutdown(void);
BOOLEAN NvmeHotInfo(int Index, char *Name, int Size, char *Detail, int DSize);

#endif
