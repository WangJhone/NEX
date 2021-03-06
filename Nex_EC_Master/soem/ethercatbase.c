/*
 * Licensed under the GNU General Public License version 2 with exceptions. See
 * LICENSE file in the project root for full license information
 */

/** \file
 * \brief
 * Base EtherCAT functions.
 *
 * Setting up a datagram in an ethernet frame.
 * EtherCAT datagram primitives, broadcast, auto increment, configured and
 * logical addressed data transfers. All base transfers are blocking, so
 * wait for the frame to be returned to the master or timeout. If this is
 * not acceptable build your own datagrams and use the functions from nicdrv.c.
 */

#include <stdio.h>
#include <string.h>
#include "oshw.h"
#include "osal.h"
#include "ethercattype.h"
#include "ethercatbase.h"

/** Write data to EtherCAT datagram.
 *
 * @param[out] datagramdata   = data part of datagram
 * @param[in]  com            = command
 * @param[in]  length         = length of databuffer
 * @param[in]  data           = databuffer to be copied into datagram
 */
static void nexx_writedatagramdata(void *datagramdata, nex_cmdtype com, uint16 length, const void * data)
{
   if (length > 0)
   {
      switch (com)
      {
         case NEX_CMD_NOP:
            /* Fall-through */
         case NEX_CMD_APRD:
            /* Fall-through */
         case NEX_CMD_FPRD:
            /* Fall-through */
         case NEX_CMD_BRD:
            /* Fall-through */
         case NEX_CMD_LRD:
            /* no data to write. initialise data so frame is in a known state */
            memset(datagramdata, 0, length);
            break;
         default:
            memcpy(datagramdata, data, length);
            break;
      }
   }
}

/** Generate and set EtherCAT datagram in a standard ethernet frame.
 *
 * @param[in] port        = port context struct
 * @param[out] frame       = framebuffer
 * @param[in]  com         = command
 * @param[in]  idx         = index used for TX and RX buffers
 * @param[in]  ADP         = Address Position
 * @param[in]  ADO         = Address Offset
 * @param[in]  length      = length of datagram excluding EtherCAT header
 * @param[in]  data        = databuffer to be copied in datagram
 * @return always 0
 */
int nexx_setupdatagram(nexx_portt *port, void *frame, uint8 com, uint8 idx, uint16 ADP, uint16 ADO, uint16 length, void *data)
{
   nex_comt *datagramP;
   uint8 *frameP;

   frameP = frame;
   /* Ethernet header is preset and fixed in frame buffers
      EtherCAT header needs to be added after that */
   datagramP = (nex_comt*)&frameP[ETH_HEADERSIZE];
   datagramP->elength = htoes(NEX_ECATTYPE + NEX_HEADERSIZE + length);
   datagramP->command = com;
   datagramP->index = idx;
   datagramP->ADP = htoes(ADP);
   datagramP->ADO = htoes(ADO);
   datagramP->dlength = htoes(length);
   nexx_writedatagramdata(&frameP[ETH_HEADERSIZE + NEX_HEADERSIZE], com, length, data);
   /* set WKC to zero */
   frameP[ETH_HEADERSIZE + NEX_HEADERSIZE + length] = 0x00;
   frameP[ETH_HEADERSIZE + NEX_HEADERSIZE + length + 1] = 0x00;
   /* set size of frame in buffer array */
   port->txbuflength[idx] = ETH_HEADERSIZE + NEX_HEADERSIZE + NEX_WKCSIZE + length;

   return 0;
}

/** Add EtherCAT datagram to a standard ethernet frame with existing datagram(s).
 *
 * @param[in] port        = port context struct
 * @param[out] frame      = framebuffer
 * @param[in]  com        = command
 * @param[in]  idx        = index used for TX and RX buffers
 * @param[in]  more       = TRUE if still more datagrams to follow
 * @param[in]  ADP        = Address Position
 * @param[in]  ADO        = Address Offset
 * @param[in]  length     = length of datagram excluding EtherCAT header
 * @param[in]  data       = databuffer to be copied in datagram
 * @return Offset to data in rx frame, usefull to retrieve data after RX.
 */
int nexx_adddatagram(nexx_portt *port, void *frame, uint8 com, uint8 idx, boolean more, uint16 ADP, uint16 ADO, uint16 length, void *data)
{
   nex_comt *datagramP;
   uint8 *frameP;
   uint16 prevlength;

   frameP = frame;
   /* copy previous frame size */
   prevlength = port->txbuflength[idx];
   datagramP = (nex_comt*)&frameP[ETH_HEADERSIZE];
   /* add new datagram to ethernet frame size */
   datagramP->elength = htoes( etohs(datagramP->elength) + NEX_HEADERSIZE + length );
   /* add "datagram follows" flag to previous subframe dlength */
   datagramP->dlength = htoes( etohs(datagramP->dlength) | NEX_DATAGRAMFOLLOWS );
   /* set new EtherCAT header position */
   datagramP = (nex_comt*)&frameP[prevlength - NEX_ELENGTHSIZE];
   datagramP->command = com;
   datagramP->index = idx;
   datagramP->ADP = htoes(ADP);
   datagramP->ADO = htoes(ADO);
   if (more)
   {
      /* this is not the last datagram to add */
      datagramP->dlength = htoes(length | NEX_DATAGRAMFOLLOWS);
   }
   else
   {
      /* this is the last datagram in the frame */
      datagramP->dlength = htoes(length);
   }
   nexx_writedatagramdata(&frameP[prevlength + NEX_HEADERSIZE - NEX_ELENGTHSIZE], com, length, data);
   /* set WKC to zero */
   frameP[prevlength + NEX_HEADERSIZE - NEX_ELENGTHSIZE + length] = 0x00;
   frameP[prevlength + NEX_HEADERSIZE - NEX_ELENGTHSIZE + length + 1] = 0x00;
   /* set size of frame in buffer array */
   port->txbuflength[idx] = prevlength + NEX_HEADERSIZE - NEX_ELENGTHSIZE + NEX_WKCSIZE + length;

   /* return offset to data in rx frame
      14 bytes smaller than tx frame due to stripping of ethernet header */
   return prevlength + NEX_HEADERSIZE - NEX_ELENGTHSIZE - ETH_HEADERSIZE;
}

/** BRW "broadcast write" primitive. Blocking.
 *
 * @param[in] port        = port context struct
 * @param[in] ADP         = Address Position, normally 0
 * @param[in] ADO         = Address Offset, slave memory address
 * @param[in] length      = length of databuffer
 * @param[in] data        = databuffer to be written to slaves
 * @param[in] timeout     = timeout in us, standard is NEX_TIMEOUTRET
 * @return Workcounter or NEX_NOFRAME
 */
int nexx_BWR (nexx_portt *port, uint16 ADP, uint16 ADO, uint16 length, void *data, int timeout)
{
   uint8 idx;
   int wkc;

   /* get fresh index */
   idx = nexx_getindex (port);
   /* setup datagram */
   nexx_setupdatagram (port, &(port->txbuf[idx]), NEX_CMD_BWR, idx, ADP, ADO, length, data);
   /* send data and wait for answer */
   wkc = nexx_srconfirm (port, idx, timeout);
   /* clear buffer status */
   nexx_setbufstat (port, idx, NEX_BUF_EMPTY);

   return wkc;
}

/** BRD "broadcast read" primitive. Blocking.
 *
 * @param[in] port        = port context struct
 * @param[in]  ADP        = Address Position, normally 0
 * @param[in]  ADO        = Address Offset, slave memory address
 * @param[in]  length     = length of databuffer
 * @param[out] data       = databuffer to put slave data in
 * @param[in]  timeout    = timeout in us, standard is NEX_TIMEOUTRET
 * @return Workcounter or NEX_NOFRAME
 */
int nexx_BRD(nexx_portt *port, uint16 ADP, uint16 ADO, uint16 length, void *data, int timeout)
{
   uint8 idx;
   int wkc;

   /* get fresh index */
   idx = nexx_getindex(port);
   /* setup datagram */
   nexx_setupdatagram(port, &(port->txbuf[idx]), NEX_CMD_BRD, idx, ADP, ADO, length, data);
   /* send data and wait for answer */
   wkc = nexx_srconfirm (port, idx, timeout);
   if (wkc > 0)
   {
      /* copy datagram to data buffer */
      memcpy(data, &(port->rxbuf[idx][NEX_HEADERSIZE]), length);
   }
   /* clear buffer status */
   nexx_setbufstat(port, idx, NEX_BUF_EMPTY);

   return wkc;
}

/** APRD "auto increment address read" primitive. Blocking.
 *
 * @param[in] port        = port context struct
 * @param[in]  ADP        = Address Position, each slave ++, slave that has 0 excecutes
 * @param[in]  ADO        = Address Offset, slave memory address
 * @param[in]  length     = length of databuffer
 * @param[out] data       = databuffer to put slave data in
 * @param[in]  timeout    = timeout in us, standard is NEX_TIMEOUTRET
 * @return Workcounter or NEX_NOFRAME
 */
int nexx_APRD(nexx_portt *port, uint16 ADP, uint16 ADO, uint16 length, void *data, int timeout)
{
   int wkc;
   uint8 idx;

   idx = nexx_getindex(port);
   nexx_setupdatagram(port, &(port->txbuf[idx]), NEX_CMD_APRD, idx, ADP, ADO, length, data);
   wkc = nexx_srconfirm(port, idx, timeout);
   if (wkc > 0)
   {
      memcpy(data, &(port->rxbuf[idx][NEX_HEADERSIZE]), length);
   }
   nexx_setbufstat(port, idx, NEX_BUF_EMPTY);

   return wkc;
}

/** APRMW "auto increment address read, multiple write" primitive. Blocking.
 *
 * @param[in] port        = port context struct
 * @param[in]  ADP        = Address Position, each slave ++, slave that has 0 reads,
 *                          following slaves write.
 * @param[in]  ADO        = Address Offset, slave memory address
 * @param[in]  length     = length of databuffer
 * @param[out] data       = databuffer to put slave data in
 * @param[in]  timeout    = timeout in us, standard is NEX_TIMEOUTRET
 * @return Workcounter or NEX_NOFRAME
 */
int nexx_ARMW(nexx_portt *port, uint16 ADP, uint16 ADO, uint16 length, void *data, int timeout)
{
   int wkc;
   uint8 idx;

   idx = nexx_getindex(port);
   nexx_setupdatagram(port, &(port->txbuf[idx]), NEX_CMD_ARMW, idx, ADP, ADO, length, data);
   wkc = nexx_srconfirm(port, idx, timeout);
   if (wkc > 0)
   {
      memcpy(data, &(port->rxbuf[idx][NEX_HEADERSIZE]), length);
   }
   nexx_setbufstat(port, idx, NEX_BUF_EMPTY);

   return wkc;
}

/** FPRMW "configured address read, multiple write" primitive. Blocking.
 *
 * @param[in] port        = port context struct
 * @param[in]  ADP        = Address Position, slave that has address reads,
 *                          following slaves write.
 * @param[in]  ADO        = Address Offset, slave memory address
 * @param[in]  length     = length of databuffer
 * @param[out] data       = databuffer to put slave data in
 * @param[in]  timeout    = timeout in us, standard is NEX_TIMEOUTRET
 * @return Workcounter or NEX_NOFRAME
 */
int nexx_FRMW(nexx_portt *port, uint16 ADP, uint16 ADO, uint16 length, void *data, int timeout)
{
   int wkc;
   uint8 idx;

   idx = nexx_getindex(port);
   nexx_setupdatagram(port, &(port->txbuf[idx]), NEX_CMD_FRMW, idx, ADP, ADO, length, data);
   wkc = nexx_srconfirm(port, idx, timeout);
   if (wkc > 0)
   {
      memcpy(data, &(port->rxbuf[idx][NEX_HEADERSIZE]), length);
   }
   nexx_setbufstat(port, idx, NEX_BUF_EMPTY);

   return wkc;
}

/** APRDw "auto increment address read" word return primitive. Blocking.
 *
 * @param[in] port        = port context struct
 * @param[in] ADP         = Address Position, each slave ++, slave that has 0 reads.
 * @param[in] ADO         = Address Offset, slave memory address
 * @param[in] timeout     = timeout in us, standard is NEX_TIMEOUTRET
 * @return word data from slave
 */
uint16 nexx_APRDw(nexx_portt *port, uint16 ADP, uint16 ADO, int timeout)
{
   uint16 w;

   w = 0;
   nexx_APRD(port, ADP, ADO, sizeof(w), &w, timeout);

   return w;
}

/** FPRD "configured address read" primitive. Blocking.
 *
 * @param[in] port        = port context struct
 * @param[in]  ADP        = Address Position, slave that has address reads.
 * @param[in]  ADO        = Address Offset, slave memory address
 * @param[in]  length     = length of databuffer
 * @param[out] data       = databuffer to put slave data in
 * @param[in]  timeout    = timeout in us, standard is NEX_TIMEOUTRET
 * @return Workcounter or NEX_NOFRAME
 */
int nexx_FPRD(nexx_portt *port, uint16 ADP, uint16 ADO, uint16 length, void *data, int timeout)
{
   int wkc;
   uint8 idx;

   idx = nexx_getindex(port);
   nexx_setupdatagram(port, &(port->txbuf[idx]), NEX_CMD_FPRD, idx, ADP, ADO, length, data);
   wkc = nexx_srconfirm(port, idx, timeout);
   if (wkc > 0)
   {
      memcpy(data, &(port->rxbuf[idx][NEX_HEADERSIZE]), length);
   }
   nexx_setbufstat(port, idx, NEX_BUF_EMPTY);

   return wkc;
}

/** FPRDw "configured address read" word return primitive. Blocking.
 *
 * @param[in] port        = port context struct
 * @param[in] ADP         = Address Position, slave that has address reads.
 * @param[in] ADO         = Address Offset, slave memory address
 * @param[in] timeout     = timeout in us, standard is NEX_TIMEOUTRET
 * @return word data from slave
 */
uint16 nexx_FPRDw(nexx_portt *port, uint16 ADP, uint16 ADO, int timeout)
{
   uint16 w;

   w = 0;
   nexx_FPRD(port, ADP, ADO, sizeof(w), &w, timeout);
   return w;
}

/** APWR "auto increment address write" primitive. Blocking.
 *
 * @param[in] port        = port context struct
 * @param[in] ADP         = Address Position, each slave ++, slave that has 0 writes.
 * @param[in] ADO         = Address Offset, slave memory address
 * @param[in] length      = length of databuffer
 * @param[in] data        = databuffer to write to slave.
 * @param[in] timeout     = timeout in us, standard is NEX_TIMEOUTRET
 * @return Workcounter or NEX_NOFRAME
 */
int nexx_APWR(nexx_portt *port, uint16 ADP, uint16 ADO, uint16 length, void *data, int timeout)
{
   uint8 idx;
   int wkc;

   idx = nexx_getindex(port);
   nexx_setupdatagram(port, &(port->txbuf[idx]), NEX_CMD_APWR, idx, ADP, ADO, length, data);
   wkc = nexx_srconfirm(port, idx, timeout);
   nexx_setbufstat(port, idx, NEX_BUF_EMPTY);

   return wkc;
}

/** APWRw "auto increment address write" word primitive. Blocking.
 *
 * @param[in] port        = port context struct
 * @param[in] ADP         = Address Position, each slave ++, slave that has 0 writes.
 * @param[in] ADO         = Address Offset, slave memory address
 * @param[in] data        = word data to write to slave.
 * @param[in] timeout     = timeout in us, standard is NEX_TIMEOUTRET
 * @return Workcounter or NEX_NOFRAME
 */
int nexx_APWRw(nexx_portt *port, uint16 ADP, uint16 ADO, uint16 data, int timeout)
{
   return nexx_APWR(port, ADP, ADO, sizeof(data), &data, timeout);
}

/** FPWR "configured address write" primitive. Blocking.
 *
 * @param[in] port        = port context struct
 * @param[in] ADP         = Address Position, slave that has address writes.
 * @param[in] ADO         = Address Offset, slave memory address
 * @param[in] length      = length of databuffer
 * @param[in] data        = databuffer to write to slave.
 * @param[in] timeout     = timeout in us, standard is NEX_TIMEOUTRET
 * @return Workcounter or NEX_NOFRAME
 */
int nexx_FPWR(nexx_portt *port, uint16 ADP, uint16 ADO, uint16 length, void *data, int timeout)
{
   int wkc;
   uint8 idx;

   idx = nexx_getindex(port);
   nexx_setupdatagram(port, &(port->txbuf[idx]), NEX_CMD_FPWR, idx, ADP, ADO, length, data);
   wkc = nexx_srconfirm(port, idx, timeout);
   nexx_setbufstat(port, idx, NEX_BUF_EMPTY);

   return wkc;
}

/** FPWR "configured address write" primitive. Blocking.
 *
 * @param[in] port        = port context struct
 * @param[in] ADP         = Address Position, slave that has address writes.
 * @param[in] ADO         = Address Offset, slave memory address
 * @param[in] data        = word to write to slave.
 * @param[in] timeout     = timeout in us, standard is NEX_TIMEOUTRET
 * @return Workcounter or NEX_NOFRAME
 */
int nexx_FPWRw(nexx_portt *port, uint16 ADP, uint16 ADO, uint16 data, int timeout)
{
   return nexx_FPWR(port, ADP, ADO, sizeof(data), &data, timeout);
}

/** LRW "logical memory read / write" primitive. Blocking.
 *
 * @param[in] port        = port context struct
 * @param[in]     LogAdr  = Logical memory address
 * @param[in]     length  = length of databuffer
 * @param[in,out] data    = databuffer to write to and read from slave.
 * @param[in]     timeout = timeout in us, standard is NEX_TIMEOUTRET
 * @return Workcounter or NEX_NOFRAME
 */
int nexx_LRW(nexx_portt *port, uint32 LogAdr, uint16 length, void *data, int timeout)
{
   uint8 idx;
   int wkc;

   idx = nexx_getindex(port);
   nexx_setupdatagram(port, &(port->txbuf[idx]), NEX_CMD_LRW, idx, LO_WORD(LogAdr), HI_WORD(LogAdr), length, data);
   wkc = nexx_srconfirm(port, idx, timeout);
   if ((wkc > 0) && (port->rxbuf[idx][NEX_CMDOFFSET] == NEX_CMD_LRW))
   {
      memcpy(data, &(port->rxbuf[idx][NEX_HEADERSIZE]), length);
   }
   nexx_setbufstat(port, idx, NEX_BUF_EMPTY);

   return wkc;
}

/** LRD "logical memory read" primitive. Blocking.
 *
 * @param[in] port        = port context struct
 * @param[in]  LogAdr     = Logical memory address
 * @param[in]  length     = length of bytes to read from slave.
 * @param[out] data       = databuffer to read from slave.
 * @param[in]  timeout    = timeout in us, standard is NEX_TIMEOUTRET
 * @return Workcounter or NEX_NOFRAME
 */
int nexx_LRD(nexx_portt *port, uint32 LogAdr, uint16 length, void *data, int timeout)
{
   uint8 idx;
   int wkc;

   idx = nexx_getindex(port);
   nexx_setupdatagram(port, &(port->txbuf[idx]), NEX_CMD_LRD, idx, LO_WORD(LogAdr), HI_WORD(LogAdr), length, data);
   wkc = nexx_srconfirm(port, idx, timeout);
   if ((wkc > 0) && (port->rxbuf[idx][NEX_CMDOFFSET]==NEX_CMD_LRD))
   {
      memcpy(data, &(port->rxbuf[idx][NEX_HEADERSIZE]), length);
   }
   nexx_setbufstat(port, idx, NEX_BUF_EMPTY);

   return wkc;
}

/** LWR "logical memory write" primitive. Blocking.
 *
 * @param[in] port        = port context struct
 * @param[in] LogAdr      = Logical memory address
 * @param[in] length      = length of databuffer
 * @param[in] data        = databuffer to write to slave.
 * @param[in] timeout     = timeout in us, standard is NEX_TIMEOUTRET
 * @return Workcounter or NEX_NOFRAME
 */
int nexx_LWR(nexx_portt *port, uint32 LogAdr, uint16 length, void *data, int timeout)
{
   uint8 idx;
   int wkc;

   idx = nexx_getindex(port);
   nexx_setupdatagram(port, &(port->txbuf[idx]), NEX_CMD_LWR, idx, LO_WORD(LogAdr), HI_WORD(LogAdr), length, data);
   wkc = nexx_srconfirm(port, idx, timeout);
   nexx_setbufstat(port, idx, NEX_BUF_EMPTY);

   return wkc;
}

/** LRW "logical memory read / write" primitive plus Clock Distribution. Blocking.
 * Frame consists of two datagrams, one LRW and one FPRMW.
 *
 * @param[in] port        = port context struct
 * @param[in]     LogAdr  = Logical memory address
 * @param[in]     length  = length of databuffer
 * @param[in,out] data    = databuffer to write to and read from slave.
 * @param[in]     DCrs    = Distributed Clock reference slave address.
 * @param[out]    DCtime  = DC time read from reference slave.
 * @param[in]     timeout = timeout in us, standard is NEX_TIMEOUTRET
 * @return Workcounter or NEX_NOFRAME
 */
int nexx_LRWDC(nexx_portt *port, uint32 LogAdr, uint16 length, void *data, uint16 DCrs, int64 *DCtime, int timeout)
{
   uint16 DCtO;
   uint8 idx;
   int wkc;
   uint64 DCtE;

   idx = nexx_getindex(port);
   /* LRW in first datagram */
   nexx_setupdatagram(port, &(port->txbuf[idx]), NEX_CMD_LRW, idx, LO_WORD(LogAdr), HI_WORD(LogAdr), length, data);
   /* FPRMW in second datagram */
   DCtE = htoell(*DCtime);
   DCtO = nexx_adddatagram(port, &(port->txbuf[idx]), NEX_CMD_FRMW, idx, FALSE, DCrs, ECT_REG_DCSYSTIME, sizeof(DCtime), &DCtE);
   wkc = nexx_srconfirm(port, idx, timeout);
   if ((wkc > 0) && (port->rxbuf[idx][NEX_CMDOFFSET] == NEX_CMD_LRW))
   {
      memcpy(data, &(port->rxbuf[idx][NEX_HEADERSIZE]), length);
      memcpy(&wkc, &(port->rxbuf[idx][NEX_HEADERSIZE + length]), NEX_WKCSIZE);
      memcpy(&DCtE, &(port->rxbuf[idx][DCtO]), sizeof(*DCtime));
      *DCtime = etohll(DCtE);
   }
   nexx_setbufstat(port, idx, NEX_BUF_EMPTY);

   return wkc;
}

#ifdef NEX_VER1
int nex_setupdatagram(void *frame, uint8 com, uint8 idx, uint16 ADP, uint16 ADO, uint16 length, void *data)
{
   return nexx_setupdatagram (&nexx_port, frame, com, idx, ADP, ADO, length, data);
}

int nex_adddatagram (void *frame, uint8 com, uint8 idx, boolean more, uint16 ADP, uint16 ADO, uint16 length, void *data)
{
   return nexx_adddatagram (&nexx_port, frame, com, idx, more, ADP, ADO, length, data);
}

int nex_BWR(uint16 ADP, uint16 ADO, uint16 length, void *data, int timeout)
{
   return nexx_BWR (&nexx_port, ADP, ADO, length, data, timeout);
}

int nex_BRD(uint16 ADP, uint16 ADO, uint16 length, void *data, int timeout)
{
   return nexx_BRD(&nexx_port, ADP, ADO, length, data, timeout);
}

int nex_APRD(uint16 ADP, uint16 ADO, uint16 length, void *data, int timeout)
{
   return nexx_APRD(&nexx_port, ADP, ADO, length, data, timeout);
}

int nex_ARMW(uint16 ADP, uint16 ADO, uint16 length, void *data, int timeout)
{
   return nexx_ARMW(&nexx_port, ADP, ADO, length, data, timeout);
}

int nex_FRMW(uint16 ADP, uint16 ADO, uint16 length, void *data, int timeout)
{
   return nexx_FRMW(&nexx_port, ADP, ADO, length, data, timeout);
}

uint16 nex_APRDw(uint16 ADP, uint16 ADO, int timeout)
{
   uint16 w;

   w = 0;
   nex_APRD(ADP, ADO, sizeof(w), &w, timeout);

   return w;
}

int nex_FPRD(uint16 ADP, uint16 ADO, uint16 length, void *data, int timeout)
{
   return nexx_FPRD(&nexx_port, ADP, ADO, length, data, timeout);
}

uint16 nex_FPRDw(uint16 ADP, uint16 ADO, int timeout)
{
   uint16 w;

   w = 0;
   nex_FPRD(ADP, ADO, sizeof(w), &w, timeout);
   return w;
}

int nex_APWR(uint16 ADP, uint16 ADO, uint16 length, void *data, int timeout)
{
   return nexx_APWR(&nexx_port, ADP, ADO, length, data, timeout);
}

int nex_APWRw(uint16 ADP, uint16 ADO, uint16 data, int timeout)
{
   return nex_APWR(ADP, ADO, sizeof(data), &data, timeout);
}

int nex_FPWR(uint16 ADP, uint16 ADO, uint16 length, void *data, int timeout)
{
   return nexx_FPWR(&nexx_port, ADP, ADO, length, data, timeout);
}

int nex_FPWRw(uint16 ADP, uint16 ADO, uint16 data, int timeout)
{
   return nex_FPWR(ADP, ADO, sizeof(data), &data, timeout);
}

int nex_LRW(uint32 LogAdr, uint16 length, void *data, int timeout)
{
   return nexx_LRW(&nexx_port, LogAdr, length, data, timeout);
}

int nex_LRD(uint32 LogAdr, uint16 length, void *data, int timeout)
{
   return nexx_LRD(&nexx_port, LogAdr, length, data, timeout);
}

int nex_LWR(uint32 LogAdr, uint16 length, void *data, int timeout)
{
   return nexx_LWR(&nexx_port, LogAdr, length, data, timeout);
}

int nex_LRWDC(uint32 LogAdr, uint16 length, void *data, uint16 DCrs, int64 *DCtime, int timeout)
{
   return nexx_LRWDC(&nexx_port, LogAdr, length, data, DCrs, DCtime, timeout);
}
#endif
