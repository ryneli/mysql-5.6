/*
   Copyright (c) 2003, 2010, Oracle and/or its affiliates. All rights reserved.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA
*/

#include <ndb_global.h>

#include <TransporterCallback.hpp>
#include <TransporterRegistry.hpp>
#include <FastScheduler.hpp>
#include <Emulator.hpp>
#include <ErrorHandlingMacros.hpp>

#include "LongSignal.hpp"
#include "LongSignalImpl.hpp"

#include <signaldata/EventReport.hpp>
#include <signaldata/TestOrd.hpp>
#include <signaldata/SignalDroppedRep.hpp>
#include <signaldata/DisconnectRep.hpp>

#include "VMSignal.hpp"
#include <NdbOut.hpp>
#include "TransporterCallbackKernel.hpp"

/**
 * The instance
 */
SectionSegmentPool g_sectionSegmentPool;

/* Instance debugging vars
 * Set from DBTC
 */
int ErrorSignalReceive= 0;
int ErrorMaxSegmentsToSeize= 0;

/**
 * This variable controls if ErrorSignalReceive/ErrorMaxSegmentsToSeize
 *   is active...This to make sure only received signals are affected
 *   and not long signals sent inside node
 */
extern bool ErrorImportActive;

struct ConnectionError
{
  enum TransporterError err;
  const char *text;
};

static const ConnectionError connectionError[] =
{
  { TE_NO_ERROR, "No error"},
  { TE_SHM_UNABLE_TO_CREATE_SEGMENT, "Unable to create shared memory segment"},
  { (enum TransporterError) -1, "No connection error message available (please report a bug)"}
};

const char *lookupConnectionError(Uint32 err)
{
  int i= 0;
  while ((Uint32)connectionError[i].err != err && 
	 connectionError[i].err != -1)
    i++;
  return connectionError[i].text;
}

#include <DebuggerNames.hpp>

#ifndef NDBD_MULTITHREADED
extern TransporterRegistry globalTransporterRegistry; // Forward declaration

class TransporterCallbackKernelNonMT : public TransporterCallbackKernel
{
  /**
   * Check to see if jobbbuffers are starting to get full
   * and if so call doJob
   */
  int checkJobBuffer() { return globalScheduler.checkDoJob(); }
  void reportSendLen(NodeId nodeId, Uint32 count, Uint64 bytes);
  Uint32 get_bytes_to_send_iovec(NodeId node, struct iovec *dst, Uint32 max)
  {
    return globalTransporterRegistry.get_bytes_to_send_iovec(node, dst, max);
  }
  Uint32 bytes_sent(NodeId node, Uint32 bytes)
  {
    return globalTransporterRegistry.bytes_sent(node, bytes);
  }
  bool has_data_to_send(NodeId node)
  {
    return globalTransporterRegistry.has_data_to_send(node);
  }
  void reset_send_buffer(NodeId node, bool should_be_empty)
  {
    globalTransporterRegistry.reset_send_buffer(node, should_be_empty);
  }
};
static TransporterCallbackKernelNonMT myTransporterCallback;
TransporterRegistry globalTransporterRegistry(&myTransporterCallback);
#endif

#ifdef NDBD_MULTITHREADED
static SectionSegmentPool::Cache cache(1024,1024);

void
mt_set_section_chunk_size()
{
  g_sectionSegmentPool.setChunkSize(256);
}

#else
void mt_set_section_chunk_size(){}
#endif

void
TransporterCallbackKernel::deliver_signal(SignalHeader * const header,
                                          Uint8 prio,
                                          Uint32 * const theData,
                                          LinearSectionPtr ptr[3])
{

  const Uint32 secCount = header->m_noOfSections;
  const Uint32 length = header->theLength;

  // if this node is not MT LQH then instance bits are stripped at execute

#ifdef TRACE_DISTRIBUTED
  ndbout_c("recv: %s(%d) from (%s, %d)",
	   getSignalName(header->theVerId_signalNumber), 
	   header->theVerId_signalNumber,
	   getBlockName(refToBlock(header->theSendersBlockRef)),
	   refToNode(header->theSendersBlockRef));
#endif

  bool ok = true;
  Ptr<SectionSegment> secPtr[3];
  bzero(secPtr, sizeof(secPtr));
  secPtr[0].p = secPtr[1].p = secPtr[2].p = 0;

  ErrorImportActive = true;
  switch(secCount){
  case 3:
    ok &= import(SPC_CACHE_ARG secPtr[2], ptr[2].p, ptr[2].sz);
  case 2:
    ok &= import(SPC_CACHE_ARG secPtr[1], ptr[1].p, ptr[1].sz);
  case 1:
    ok &= import(SPC_CACHE_ARG secPtr[0], ptr[0].p, ptr[0].sz);
  }
  ErrorImportActive = false;

  /**
   * Check that we haven't received a too long signal
   */
  ok &= (length + secCount <= 25);
  
  Uint32 secPtrI[3];
  if(ok){
    /**
     * Normal path 
     */
    secPtrI[0] = secPtr[0].i;
    secPtrI[1] = secPtr[1].i;
    secPtrI[2] = secPtr[2].i;

#ifndef NDBD_MULTITHREADED
    globalScheduler.execute(header, prio, theData, secPtrI);  
#else
    if (prio == JBB)
      sendlocal(receiverThreadId,
                header, theData, secPtrI);
    else
      sendprioa(receiverThreadId,
                header, theData, secPtrI);

#endif
    return;
  }
  
  /**
   * Out of memory
   */
  for(Uint32 i = 0; i<secCount; i++){
    if(secPtr[i].p != 0){
      g_sectionSegmentPool.releaseList(SPC_SEIZE_ARG
                                       relSz(secPtr[i].p->m_sz),
                                       secPtr[i].i, 
				       secPtr[i].p->m_lastSegment);
    }
  }

  SignalDroppedRep * rep = (SignalDroppedRep*)theData;
  Uint32 gsn = header->theVerId_signalNumber;
  Uint32 len = header->theLength;
  Uint32 newLen= (len > 22 ? 22 : len);
  memmove(rep->originalData, theData, (4 * newLen));
  rep->originalGsn = gsn;
  rep->originalLength = len;
  rep->originalSectionCount = secCount;
  header->theVerId_signalNumber = GSN_SIGNAL_DROPPED_REP;
  header->theLength = newLen + 3;
  header->m_noOfSections = 0;
#ifndef NDBD_MULTITHREADED
  globalScheduler.execute(header, prio, theData, secPtrI);    
#else
  if (prio == JBB)
    sendlocal(receiverThreadId,
              header, theData, NULL);
  else
    sendprioa(receiverThreadId,
              header, theData, NULL);
    
#endif
}

NdbOut & 
operator<<(NdbOut& out, const SectionSegment & ss){
  out << "[ last= " << ss.m_lastSegment << " next= " << ss.nextPool << " ]";
  return out;
}

void
TransporterCallbackKernel::reportError(NodeId nodeId,
                                       TransporterError errorCode,
                                       const char *info)
{
#ifdef DEBUG_TRANSPORTER
  ndbout_c("reportError (%d, 0x%x) %s", nodeId, errorCode, info ? info : "");
#endif

  DBUG_ENTER("reportError");
  DBUG_PRINT("info",("nodeId %d  errorCode: 0x%x  info: %s",
		     nodeId, errorCode, info));

  switch (errorCode)
  {
  case TE_SIGNAL_LOST_SEND_BUFFER_FULL:
  {
    char msg[64];
    BaseString::snprintf(msg, sizeof(msg), "Remote node id %d.%s%s", nodeId,
	     info ? " " : "", info ? info : "");
    ErrorReporter::handleError(NDBD_EXIT_SIGNAL_LOST_SEND_BUFFER_FULL,
			       msg, __FILE__, NST_ErrorHandler);
  }
  case TE_SIGNAL_LOST:
  {
    char msg[64];
    BaseString::snprintf(msg, sizeof(msg), "Remote node id %d,%s%s", nodeId,
	     info ? " " : "", info ? info : "");
    ErrorReporter::handleError(NDBD_EXIT_SIGNAL_LOST,
			       msg, __FILE__, NST_ErrorHandler);
  }
  case TE_SHM_IPC_PERMANENT:
  {
    char msg[128];
    BaseString::snprintf(msg, sizeof(msg),
	     "Remote node id %d.%s%s",
	     nodeId, info ? " " : "", info ? info : "");
    ErrorReporter::handleError(NDBD_EXIT_CONNECTION_SETUP_FAILED,
			       msg, __FILE__, NST_ErrorHandler);
  }
  default:
    break;
  }
 
  if(errorCode & TE_DO_DISCONNECT){
    reportDisconnect(nodeId, errorCode);
  }
  
  SignalT<3> signal;
  memset(&signal.header, 0, sizeof(signal.header));


  if(errorCode & TE_DO_DISCONNECT)
    signal.theData[0] = NDB_LE_TransporterError;
  else
    signal.theData[0] = NDB_LE_TransporterWarning;
  
  signal.theData[1] = nodeId;
  signal.theData[2] = errorCode;
  
  signal.header.theLength = 3;  
  signal.header.theSendersSignalId = 0;
  signal.header.theSendersBlockRef = numberToRef(0, globalData.ownId);
  signal.header.theReceiversBlockNumber = CMVMI;
  signal.header.theVerId_signalNumber = GSN_EVENT_REP;
#ifndef NDBD_MULTITHREADED
  Uint32 secPtr[3];
  globalScheduler.execute(&signal.header, JBA, signal.theData, secPtr);
#else
  sendprioa(receiverThreadId,
            &signal.header, signal.theData, NULL);
#endif

  DBUG_VOID_RETURN;
}

/**
 * Report average send length in bytes (4096 last sends)
 */
#ifndef NDBD_MULTITHREADED
void
TransporterCallbackKernelNonMT::reportSendLen(NodeId nodeId, Uint32 count,
                                              Uint64 bytes)
{

  SignalT<3> signal;
  memset(&signal.header, 0, sizeof(signal.header));

  signal.header.theLength = 3;
  signal.header.theSendersSignalId = 0;
  signal.header.theSendersBlockRef = numberToRef(0, globalData.ownId);
  signal.header.theReceiversBlockNumber = CMVMI;
  signal.header.theVerId_signalNumber = GSN_EVENT_REP;

  signal.theData[0] = NDB_LE_SendBytesStatistic;
  signal.theData[1] = nodeId;
  signal.theData[2] = Uint32(bytes/count);

  Uint32 secPtr[3];
  globalScheduler.execute(&signal.header, JBA, signal.theData, secPtr);
}
#endif

/**
 * Report average receive length in bytes (4096 last receives)
 */
void
TransporterCallbackKernel::reportReceiveLen(NodeId nodeId, Uint32 count,
                                            Uint64 bytes)
{

  SignalT<3> signal;
  memset(&signal.header, 0, sizeof(signal.header));

  signal.header.theLength = 3;  
  signal.header.theSendersSignalId = 0;
  signal.header.theSendersBlockRef = numberToRef(0, globalData.ownId);
  signal.header.theReceiversBlockNumber = CMVMI;
  signal.header.theVerId_signalNumber = GSN_EVENT_REP;

  signal.theData[0] = NDB_LE_ReceiveBytesStatistic;
  signal.theData[1] = nodeId;
  signal.theData[2] = Uint32(bytes/count);
#ifndef NDBD_MULTITHREADED
  Uint32 secPtr[3];
  globalScheduler.execute(&signal.header, JBA, signal.theData, secPtr);
#else
  sendprioa(receiverThreadId,
            &signal.header, signal.theData, NULL);
#endif
}

/**
 * Report connection established
 */

void
TransporterCallbackKernel::reportConnect(NodeId nodeId)
{

  SignalT<1> signal;
  memset(&signal.header, 0, sizeof(signal.header));

  signal.header.theLength = 1; 
  signal.header.theSendersSignalId = 0;
  signal.header.theSendersBlockRef = numberToRef(0, globalData.ownId);
  signal.header.theReceiversBlockNumber = CMVMI;
  signal.header.theVerId_signalNumber = GSN_CONNECT_REP;

  signal.theData[0] = nodeId;
  
#ifndef NDBD_MULTITHREADED
  Uint32 secPtr[3];
  globalScheduler.execute(&signal.header, JBA, signal.theData, secPtr);
#else
  sendprioa(receiverThreadId,
            &signal.header, signal.theData, NULL);
#endif
}

/**
 * Report connection broken
 */
void
TransporterCallbackKernel::reportDisconnect(NodeId nodeId, Uint32 errNo)
{

  DBUG_ENTER("reportDisconnect");

  SignalT<sizeof(DisconnectRep)/4> signal;
  memset(&signal.header, 0, sizeof(signal.header));

  signal.header.theLength = DisconnectRep::SignalLength; 
  signal.header.theSendersSignalId = 0;
  signal.header.theSendersBlockRef = numberToRef(0, globalData.ownId);
  signal.header.theTrace = TestOrd::TraceDisconnect;
  signal.header.theVerId_signalNumber = GSN_DISCONNECT_REP;
  signal.header.theReceiversBlockNumber = CMVMI;

  DisconnectRep * rep = CAST_PTR(DisconnectRep, &signal.theData[0]);
  rep->nodeId = nodeId;
  rep->err = errNo;

#ifndef NDBD_MULTITHREADED
  Uint32 secPtr[3];
  globalScheduler.execute(&signal.header, JBA, signal.theData, secPtr);
#else
  sendprioa(receiverThreadId,
            &signal.header, signal.theData, NULL);
#endif

  DBUG_VOID_RETURN;
}

void
SignalLoggerManager::printSegmentedSection(FILE * output,
                                           const SignalHeader & sh,
                                           const SegmentedSectionPtr ptr[3],
                                           unsigned i)
{
  fprintf(output, "SECTION %u type=segmented", i);
  if (i >= 3) {
    fprintf(output, " *** invalid ***\n");
    return;
  }
  const Uint32 len = ptr[i].sz;
  SectionSegment * ssp = ptr[i].p;
  Uint32 pos = 0;
  fprintf(output, " size=%u\n", (unsigned)len);
  while (pos < len) {
    if (pos > 0 && pos % SectionSegment::DataLength == 0) {
      ssp = g_sectionSegmentPool.getPtr(ssp->m_nextSegment);
    }
    printDataWord(output, pos, ssp->theData[pos % SectionSegment::DataLength]);
  }
  if (len > 0)
    putc('\n', output);
}

void
TransporterCallbackKernel::transporter_recv_from(NodeId nodeId)
{
  globalData.m_nodeInfo[nodeId].m_heartbeat_cnt= 0;
  return;
}
