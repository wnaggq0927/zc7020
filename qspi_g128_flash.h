#ifndef QSPI_G128_FLASH_H_
#define QSPI_G128_FLASH_H_

#include "xqspips.h"

int Init_qspi(XQspiPs *QspiInstancePtr, u16 QspiDeviceId);
void update_flash(u8 *buffer, u8 *read_buffer, u8 *write_buffer, u32 length);
int update_flash_at(u32 flash_address, u8 *buffer, u8 *read_buffer,
                    u8 *write_buffer, u32 length);
void read_flash_at(u32 flash_address, u8 *buffer, u8 *read_work_buffer,
                   u32 length);
void FlashErase(XQspiPs *QspiPtr, u32 Address, u32 ByteCount, u8 *WriteBfrPtr);

void FlashWrite(XQspiPs *QspiPtr, u32 Address, u32 ByteCount, u8 Command,
				u8 *WriteBfrPtr);

int FlashReadID(XQspiPs *QspiPtr, u8 *WriteBfrPtr, u8 *ReadBfrPtr);

void FlashRead(XQspiPs *QspiPtr, u32 Address, u32 ByteCount, u8 Command,
				u8 *WriteBfrPtr, u8 *ReadBfrPtr);

int SendBankSelect(XQspiPs *QspiPtr, u8 *WriteBfrPtr, u32 BankSel);

void BulkErase(XQspiPs *QspiPtr, u8 *WriteBfrPtr);

void DieErase(XQspiPs *QspiPtr, u8 *WriteBfrPtr);

u32 GetRealAddr(XQspiPs *QspiPtr, u32 Address);

#endif /* QSPI_G128_FLASH_H_ */
