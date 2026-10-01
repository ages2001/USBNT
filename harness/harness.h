#ifndef HARNESS_H
#define HARNESS_H
void SerialInit(void);
void SerialPut(char c);
int  SerialGet(void);
void HarnessPrintf(const char *Fmt, ...);
void OsInitHarness(void);
void HarnessSetConfig(const char *Name, ULONG Value);
extern volatile ULONG HarnessWorkerWake;
#endif
