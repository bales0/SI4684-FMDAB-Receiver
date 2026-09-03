#ifndef IR_REMOTE_H
#define IR_REMOTE_H

#include <Arduino.h>

bool IrRemotePrepare(void);  // one-time heap allocation for persistent IR tables
void IrRemoteBegin(void);
void IrRemoteStop(void);
void IrRemoteProcess(void);

void IrRemoteUiOpen(void);
void IrRemoteUiRotate(int8_t direction);
bool IrRemoteUiPress(void);   // true = return to main Settings menu
void IrRemoteUiAbort(void);
bool IrRemoteUiActive(void);
bool IrRemoteHasProfile(void);

#endif
