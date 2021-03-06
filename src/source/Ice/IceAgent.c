/**
 * Kinesis Video Producer Callbacks Provider
 */
#define LOG_CLASS "IceAgent"
#include "../Include_i.h"

extern StateMachineState ICE_AGENT_STATE_MACHINE_STATES[];
extern UINT32 ICE_AGENT_STATE_MACHINE_STATE_COUNT;

STATUS createIceAgent(PCHAR username, PCHAR password, PIceAgentCallbacks pIceAgentCallbacks,
                      PRtcConfiguration pRtcConfiguration, TIMER_QUEUE_HANDLE timerQueueHandle,
                      PConnectionListener pConnectionListener, PIceAgent* ppIceAgent)
{
    ENTERS();

    STATUS retStatus = STATUS_SUCCESS;
    PIceAgent pIceAgent = NULL;
    UINT32 i;

    CHK(ppIceAgent != NULL && username != NULL && password != NULL && pConnectionListener != NULL, STATUS_NULL_ARG);
    CHK(STRNLEN(username, MAX_ICE_CONFIG_USER_NAME_LEN + 1) <= MAX_ICE_CONFIG_USER_NAME_LEN &&
        STRNLEN(password, MAX_ICE_CONFIG_CREDENTIAL_LEN + 1) <= MAX_ICE_CONFIG_CREDENTIAL_LEN, STATUS_INVALID_ARG);

    // allocate the entire struct
    pIceAgent = (PIceAgent) MEMCALLOC(1, SIZEOF(IceAgent));
    STRNCPY(pIceAgent->localUsername, username, MAX_ICE_CONFIG_USER_NAME_LEN);
    STRNCPY(pIceAgent->localPassword, password, MAX_ICE_CONFIG_CREDENTIAL_LEN);

    // candidatePairs start off being invalid since we MEMCALLOC pIceAgent
    ATOMIC_STORE_BOOL(&pIceAgent->remoteCredentialReceived, FALSE);
    ATOMIC_STORE_BOOL(&pIceAgent->agentStartGathering, FALSE);
    ATOMIC_STORE_BOOL(&pIceAgent->candidateGatheringFinished, FALSE);
    pIceAgent->isControlling = FALSE;
    pIceAgent->tieBreaker = (UINT64) RAND();
    pIceAgent->iceTransportPolicy = pRtcConfiguration->iceTransportPolicy;
    pIceAgent->kvsRtcConfiguration = pRtcConfiguration->kvsRtcConfiguration;
    CHK_STATUS(iceAgentValidateKvsRtcConfig(&pIceAgent->kvsRtcConfiguration));

    if (pIceAgentCallbacks != NULL) {
        pIceAgent->iceAgentCallbacks = *pIceAgentCallbacks;
    }
    pIceAgent->stateEndTime = 0;
    pIceAgent->foundationCounter = 0;
    pIceAgent->localNetworkInterfaceCount = ARRAY_SIZE(pIceAgent->localNetworkInterfaces);
    pIceAgent->candidateGatheringEndTime = INVALID_TIMESTAMP_VALUE;

    pIceAgent->lock = MUTEX_CREATE(FALSE);

    // Create the state machine
    CHK_STATUS(createStateMachine(ICE_AGENT_STATE_MACHINE_STATES,
                                  ICE_AGENT_STATE_MACHINE_STATE_COUNT,
                                  (UINT64) pIceAgent,
                                  kinesisVideoStreamDefaultGetCurrentTime,
                                  (UINT64) pIceAgent,
                                  &pIceAgent->pStateMachine));
    pIceAgent->iceAgentStatus = STATUS_SUCCESS;
    pIceAgent->iceAgentStateTimerTask = UINT32_MAX;
    pIceAgent->keepAliveTimerTask = UINT32_MAX;
    pIceAgent->iceCandidateGatheringTimerTask = UINT32_MAX;
    pIceAgent->timerQueueHandle = timerQueueHandle;
    pIceAgent->lastDataReceivedTime = INVALID_TIMESTAMP_VALUE;
    pIceAgent->detectedDisconnection = FALSE;
    pIceAgent->disconnectionGracePeriodEndTime = INVALID_TIMESTAMP_VALUE;
    pIceAgent->pConnectionListener = pConnectionListener;
    CHK_STATUS(createTransactionIdStore(DEFAULT_MAX_STORED_TRANSACTION_ID_COUNT,
                                        &pIceAgent->pStunBindingRequestTransactionIdStore));

    pIceAgent->turnConnectionTrackerCount = 0;

    CHK_STATUS(doubleListCreate(&pIceAgent->localCandidates));
    CHK_STATUS(doubleListCreate(&pIceAgent->remoteCandidates));
    CHK_STATUS(doubleListCreate(&pIceAgent->iceCandidatePairs));
    CHK_STATUS(stackQueueCreate(&pIceAgent->triggeredCheckQueue));

    // Pre-allocate stun packets

    // no other attribtues needed: https://tools.ietf.org/html/rfc8445#section-11
    CHK_STATUS(createStunPacket(STUN_PACKET_TYPE_BINDING_INDICATION,
                                NULL,
                                &pIceAgent->pBindingIndication));

    pIceAgent->iceServersCount = 0;
    for (i = 0; i < MAX_ICE_SERVERS_COUNT; i++) {
        if (pRtcConfiguration->iceServers[i].urls[0] != '\0' &&
            STATUS_SUCCEEDED(parseIceServer(&pIceAgent->iceServers[pIceAgent->iceServersCount],
                                            (PCHAR) pRtcConfiguration->iceServers[i].urls,
                                            (PCHAR) pRtcConfiguration->iceServers[i].username,
                                            (PCHAR) pRtcConfiguration->iceServers[i].credential))) {
            pIceAgent->iceServersCount++;
        }
    }

CleanUp:

    if (STATUS_FAILED(retStatus) && pIceAgent != NULL) {
        freeIceAgent(&pIceAgent);
        pIceAgent = NULL;
    }

    if (ppIceAgent != NULL) {
        *ppIceAgent = pIceAgent;
    }

    LEAVES();
    return retStatus;
}

/**
 * Not thread-safe
 * @param ppIceAgent
 * @return
 */
STATUS freeIceAgent(PIceAgent* ppIceAgent)
{
    ENTERS();

    STATUS retStatus = STATUS_SUCCESS;
    PIceAgent pIceAgent = NULL;
    PDoubleListNode pCurNode = NULL;
    UINT64 data;
    PIceCandidatePair pIceCandidatePair = NULL;

    CHK(ppIceAgent != NULL, STATUS_NULL_ARG);
    // freeIceAgent is idempotent
    CHK(*ppIceAgent != NULL, retStatus);

    pIceAgent = *ppIceAgent;

    if (pIceAgent->pConnectionListener != NULL) {
        CHK_LOG_ERR(freeConnectionListener(&pIceAgent->pConnectionListener));
    }

    if (pIceAgent->iceCandidatePairs != NULL) {
        CHK_STATUS(doubleListGetHeadNode(pIceAgent->iceCandidatePairs, &pCurNode));
        while (pCurNode != NULL) {
            CHK_STATUS(doubleListGetNodeData(pCurNode, &data));
            pCurNode = pCurNode->pNext;
            pIceCandidatePair = (PIceCandidatePair) data;

            CHK_LOG_ERR(freeIceCandidatePair(&pIceCandidatePair));
        }

        CHK_LOG_ERR(doubleListClear(pIceAgent->iceCandidatePairs, FALSE));
        CHK_LOG_ERR(doubleListFree(pIceAgent->iceCandidatePairs));
    }

    if (pIceAgent->localCandidates != NULL) {
        // socketConnections already freed by connectionListener
        // free all stored candidates
        CHK_LOG_ERR(doubleListClear(pIceAgent->localCandidates, TRUE));
        CHK_LOG_ERR(doubleListFree(pIceAgent->localCandidates));
    }

    if (pIceAgent->remoteCandidates != NULL) {
        // remote candidates dont have socketConnection
        CHK_LOG_ERR(doubleListClear(pIceAgent->remoteCandidates, TRUE));
        CHK_LOG_ERR(doubleListFree(pIceAgent->remoteCandidates));
    }

    if (pIceAgent->triggeredCheckQueue != NULL) {
        CHK_LOG_ERR(stackQueueFree(pIceAgent->triggeredCheckQueue));
    }

    if (IS_VALID_MUTEX_VALUE(pIceAgent->lock)) {
        MUTEX_FREE(pIceAgent->lock);
    }

    freeStateMachine(pIceAgent->pStateMachine);

    if (pIceAgent->pBindingIndication != NULL) {
        freeStunPacket(&pIceAgent->pBindingIndication);
    }

    if (pIceAgent->pBindingRequest != NULL) {
        freeStunPacket(&pIceAgent->pBindingRequest);
    }

    if (pIceAgent->pStunBindingRequestTransactionIdStore != NULL) {
        freeTransactionIdStore(&pIceAgent->pStunBindingRequestTransactionIdStore);
    }

    MEMFREE(pIceAgent);

    *ppIceAgent = NULL;

CleanUp:

    LEAVES();
    return retStatus;
}

STATUS iceAgentValidateKvsRtcConfig(PKvsRtcConfiguration pKvsRtcConfiguration)
{
    STATUS retStatus = STATUS_SUCCESS;

    CHK(pKvsRtcConfiguration != NULL, STATUS_NULL_ARG);

    if (pKvsRtcConfiguration->iceLocalCandidateGatheringTimeout == 0) {
        pKvsRtcConfiguration->iceLocalCandidateGatheringTimeout = KVS_ICE_GATHER_REFLEXIVE_AND_RELAYED_CANDIDATE_TIMEOUT;
    }

    if (pKvsRtcConfiguration->iceConnectionCheckTimeout == 0) {
        pKvsRtcConfiguration->iceConnectionCheckTimeout = KVS_ICE_CONNECTIVITY_CHECK_TIMEOUT;
    }

    if (pKvsRtcConfiguration->iceCandidateNominationTimeout == 0) {
        pKvsRtcConfiguration->iceCandidateNominationTimeout = KVS_ICE_CANDIDATE_NOMINATION_TIMEOUT;
    }

    if (pKvsRtcConfiguration->iceConnectionCheckPollingInterval == 0) {
        pKvsRtcConfiguration->iceConnectionCheckPollingInterval = KVS_ICE_CONNECTION_CHECK_POLLING_INTERVAL;
    }

    DLOGD("\n\ticeLocalCandidateGatheringTimeout: %u ms"
          "\n\ticeConnectionCheckTimeout: %u ms"
          "\n\ticeCandidateNominationTimeout: %u ms"
          "\n\ticeConnectionCheckPollingInterval: %u ms",
          pKvsRtcConfiguration->iceLocalCandidateGatheringTimeout / HUNDREDS_OF_NANOS_IN_A_MILLISECOND,
          pKvsRtcConfiguration->iceConnectionCheckTimeout / HUNDREDS_OF_NANOS_IN_A_MILLISECOND,
          pKvsRtcConfiguration->iceCandidateNominationTimeout / HUNDREDS_OF_NANOS_IN_A_MILLISECOND,
          pKvsRtcConfiguration->iceConnectionCheckPollingInterval / HUNDREDS_OF_NANOS_IN_A_MILLISECOND);

CleanUp:

    return retStatus;
}

STATUS iceAgentReportNewLocalCandidate(PIceAgent pIceAgent, PIceCandidate pIceCandidate)
{
    ENTERS();
    STATUS retStatus = STATUS_SUCCESS;
    CHAR serializedIceCandidateBuf[MAX_SDP_ATTRIBUTE_VALUE_LENGTH];
    UINT32 serializedIceCandidateBufLen = ARRAY_SIZE(serializedIceCandidateBuf);

    CHK(pIceAgent != NULL && pIceCandidate != NULL, STATUS_NULL_ARG);
    CHK_WARN(pIceAgent->iceAgentCallbacks.newLocalCandidateFn != NULL, retStatus, "newLocalCandidateFn callback not implemented");
    CHK_WARN(!ATOMIC_LOAD_BOOL(&pIceAgent->candidateGatheringFinished), retStatus,
                     "Cannot report new ice candidate because candidate gathering is already finished");
    CHK_STATUS(iceCandidateSerialize(pIceCandidate, serializedIceCandidateBuf, &serializedIceCandidateBufLen));
    pIceAgent->iceAgentCallbacks.newLocalCandidateFn(pIceAgent->iceAgentCallbacks.customData, serializedIceCandidateBuf);

CleanUp:

    CHK_LOG_ERR(retStatus);

    LEAVES();
    return retStatus;
}

STATUS iceAgentAddRemoteCandidate(PIceAgent pIceAgent, PCHAR pIceCandidateString)
{
    ENTERS();
    STATUS retStatus = STATUS_SUCCESS;
    BOOL locked = FALSE;
    PIceCandidate pIceCandidate = NULL, pDuplicatedIceCandidate = NULL;
    PCHAR curr, tail, next;
    UINT32 tokenLen, portValue, remoteCandidateCount, i;
    BOOL foundIpAndPort = FALSE, freeIceCandidateIfFail = TRUE;
    KvsIpAddress candidateIpAddr;

    CHK(pIceAgent != NULL && pIceCandidateString != NULL, STATUS_NULL_ARG);
    CHK(!IS_EMPTY_STRING(pIceCandidateString), STATUS_INVALID_ARG);

    MEMSET(&candidateIpAddr, 0x00, SIZEOF(KvsIpAddress));

    MUTEX_LOCK(pIceAgent->lock);
    locked = TRUE;

    CHK_STATUS(doubleListGetNodeCount(pIceAgent->remoteCandidates, &remoteCandidateCount));
    CHK(remoteCandidateCount < KVS_ICE_MAX_REMOTE_CANDIDATE_COUNT, STATUS_ICE_MAX_REMOTE_CANDIDATE_COUNT_EXCEEDED);

    curr = pIceCandidateString;
    tail = pIceCandidateString + STRLEN(pIceCandidateString);
    while ((next = STRNCHR(curr, tail - curr, ' ')) != NULL && !foundIpAndPort) {
        tokenLen = (UINT32) (next - curr);
        CHK(STRNCMPI("tcp", curr, tokenLen) != 0, STATUS_ICE_CANDIDATE_STRING_IS_TCP);

        if (candidateIpAddr.address[0] != 0) {
            CHK_STATUS(STRTOUI32(curr, curr + tokenLen, 10, &portValue));

            candidateIpAddr.port = htons(portValue);
            candidateIpAddr.family = KVS_IP_FAMILY_TYPE_IPV4;
        } else if (STRNCHR(curr, tokenLen, '.') != NULL) {
            CHK(tokenLen <= KVS_MAX_IPV4_ADDRESS_STRING_LEN, STATUS_ICE_CANDIDATE_STRING_INVALID_IP); // IPv4 is 15 characters at most
            CHK_STATUS(populateIpFromString(&candidateIpAddr, curr));
        }

        curr = next + 1;
        foundIpAndPort = (candidateIpAddr.port != 0) && (candidateIpAddr.address[0] != 0);
    }

    CHK(candidateIpAddr.port != 0, STATUS_ICE_CANDIDATE_STRING_MISSING_PORT);
    CHK(candidateIpAddr.address[0] != 0, STATUS_ICE_CANDIDATE_STRING_MISSING_IP);

    CHK_STATUS(findCandidateWithIp(&candidateIpAddr, pIceAgent->remoteCandidates, &pDuplicatedIceCandidate));
    CHK(pDuplicatedIceCandidate == NULL, retStatus);

    CHK((pIceCandidate = MEMCALLOC(1, SIZEOF(IceCandidate))) != NULL, STATUS_NOT_ENOUGH_MEMORY);
    pIceCandidate->ipAddress = candidateIpAddr;
    pIceCandidate->state = ICE_CANDIDATE_STATE_VALID;
    CHK_STATUS(doubleListInsertItemHead(pIceAgent->remoteCandidates, (UINT64) pIceCandidate));
    freeIceCandidateIfFail = FALSE;

    CHK_STATUS(createIceCandidatePairs(pIceAgent, pIceCandidate, TRUE));

    /* pass remote candidate to each turnConnection */
    for(i = 0; i < pIceAgent->turnConnectionTrackerCount; ++i) {
        CHK_STATUS(turnConnectionAddPeer(pIceAgent->turnConnectionTrackers[i].pTurnConnection, &pIceCandidate->ipAddress));
    }

CleanUp:

    if (locked) {
        MUTEX_UNLOCK(pIceAgent->lock);
    }

    if (STATUS_FAILED(retStatus) && freeIceCandidateIfFail) {
        SAFE_MEMFREE(pIceCandidate);
    }

    CHK_LOG_ERR(retStatus);

    LEAVES();
    return retStatus;
}

STATUS iceAgentInitHostCandidate(PIceAgent pIceAgent)
{
    ENTERS();

    STATUS retStatus = STATUS_SUCCESS;
    PKvsIpAddress pIpAddress = NULL;
    PIceCandidate pTmpIceCandidate = NULL, pDuplicatedIceCandidate = NULL, pNewIceCandidate = NULL;
    UINT32 i, localCandidateCount = 0;
    PSocketConnection pSocketConnection = NULL;
    BOOL locked = FALSE;

    for(i = 0; i < pIceAgent->localNetworkInterfaceCount; ++i) {
        pIpAddress = &pIceAgent->localNetworkInterfaces[i];

        // make sure pIceAgent->localCandidates has no duplicates
        CHK_STATUS(findCandidateWithIp(pIpAddress, pIceAgent->localCandidates, &pDuplicatedIceCandidate));

        if (pIpAddress->family == KVS_IP_FAMILY_TYPE_IPV4 && // Disable ipv6 gathering for now
            pDuplicatedIceCandidate == NULL &&
            STATUS_SUCCEEDED(createSocketConnection(pIpAddress, NULL, KVS_SOCKET_PROTOCOL_UDP, (UINT64) pIceAgent,
                                                    incomingDataHandler, pIceAgent->kvsRtcConfiguration.sendBufSize, &pSocketConnection))) {
            pTmpIceCandidate = MEMCALLOC(1, SIZEOF(IceCandidate));
            pTmpIceCandidate->ipAddress = *pIpAddress;
            pTmpIceCandidate->iceCandidateType = ICE_CANDIDATE_TYPE_HOST;
            pTmpIceCandidate->state = ICE_CANDIDATE_STATE_VALID;
            // we dont generate candidates that have the same foundation.
            pTmpIceCandidate->foundation = pIceAgent->foundationCounter++;
            pTmpIceCandidate->pSocketConnection = pSocketConnection;
            pTmpIceCandidate->priority = computeCandidatePriority(pTmpIceCandidate);

            /* Another thread could be calling iceAgentAddRemoteCandidate which triggers createIceCandidatePairs.
             * createIceCandidatePairs will read through localCandidates, since we are mutating localCandidates here,
             * need to acquire lock. */
            MUTEX_LOCK(pIceAgent->lock);
            locked = TRUE;

            CHK_STATUS(doubleListInsertItemHead(pIceAgent->localCandidates, (UINT64) pTmpIceCandidate));
            CHK_STATUS(createIceCandidatePairs(pIceAgent, pTmpIceCandidate, FALSE));

            MUTEX_UNLOCK(pIceAgent->lock);
            locked = FALSE;

            localCandidateCount++;
            // make a copy of pTmpIceCandidate so that if iceAgentReportNewLocalCandidate fails pTmpIceCandidate wont get freed.
            pNewIceCandidate = pTmpIceCandidate;
            pTmpIceCandidate = NULL;

            ATOMIC_STORE_BOOL(&pSocketConnection->receiveData, TRUE);
            // connectionListener will free the pSocketConnection at the end.
            CHK_STATUS(connectionListenerAddConnection(pIceAgent->pConnectionListener, pNewIceCandidate->pSocketConnection));
            CHK_STATUS(iceAgentReportNewLocalCandidate(pIceAgent, pNewIceCandidate));
        }
    }

    CHK(localCandidateCount != 0, STATUS_ICE_NO_LOCAL_HOST_CANDIDATE_AVAILABLE);

CleanUp:

    CHK_LOG_ERR(retStatus);

    if (locked) {
        MUTEX_UNLOCK(pIceAgent->lock);
    }

    SAFE_MEMFREE(pTmpIceCandidate);

    if (STATUS_FAILED(retStatus)) {
        iceAgentFatalError(pIceAgent, retStatus);
    }

    LEAVES();
    return retStatus;
}

STATUS iceAgentStartAgent(PIceAgent pIceAgent, PCHAR remoteUsername, PCHAR remotePassword, BOOL isControlling)
{
    ENTERS();

    STATUS retStatus = STATUS_SUCCESS;
    BOOL locked = FALSE;

    CHK(pIceAgent != NULL && remoteUsername != NULL && remotePassword != NULL, STATUS_NULL_ARG);
    CHK(!ATOMIC_LOAD_BOOL(&pIceAgent->remoteCredentialReceived), retStatus); // make iceAgentStartAgent idempotent
    CHK(STRNLEN(remoteUsername, MAX_ICE_CONFIG_USER_NAME_LEN + 1) <= MAX_ICE_CONFIG_USER_NAME_LEN &&
        STRNLEN(remotePassword, MAX_ICE_CONFIG_CREDENTIAL_LEN + 1) <= MAX_ICE_CONFIG_CREDENTIAL_LEN, STATUS_INVALID_ARG);

    MUTEX_LOCK(pIceAgent->lock);
    locked = TRUE;

    ATOMIC_STORE_BOOL(&pIceAgent->remoteCredentialReceived, TRUE);
    pIceAgent->isControlling = isControlling;

    STRNCPY(pIceAgent->remoteUsername, remoteUsername, MAX_ICE_CONFIG_USER_NAME_LEN);
    STRNCPY(pIceAgent->remotePassword, remotePassword, MAX_ICE_CONFIG_CREDENTIAL_LEN);
    if (STRLEN(pIceAgent->remoteUsername) + STRLEN(pIceAgent->localUsername) + 1 > MAX_ICE_CONFIG_USER_NAME_LEN) {
        DLOGW("remoteUsername:localUsername will be truncated to stay within %u char limit", MAX_ICE_CONFIG_USER_NAME_LEN);
    }
    SNPRINTF(pIceAgent->combinedUserName, ARRAY_SIZE(pIceAgent->combinedUserName), "%s:%s", pIceAgent->remoteUsername, pIceAgent->localUsername);

    MUTEX_UNLOCK(pIceAgent->lock);
    locked = FALSE;

    CHK_STATUS(timerQueueAddTimer(pIceAgent->timerQueueHandle,
                                  KVS_ICE_DEFAULT_TIMER_START_DELAY,
                                  pIceAgent->kvsRtcConfiguration.iceConnectionCheckPollingInterval,
                                  iceAgentStateTransitionTimerCallback,
                                  (UINT64) pIceAgent,
                                  &pIceAgent->iceAgentStateTimerTask));

CleanUp:

    if (locked) {
        MUTEX_UNLOCK(pIceAgent->lock);
    }

    LEAVES();
    return retStatus;
}

STATUS iceAgentStartGathering(PIceAgent pIceAgent)
{
    STATUS retStatus = STATUS_SUCCESS;

    CHK(pIceAgent != NULL, STATUS_NULL_ARG);
    CHK(!ATOMIC_LOAD_BOOL(&pIceAgent->agentStartGathering), retStatus);

    ATOMIC_STORE_BOOL(&pIceAgent->agentStartGathering, TRUE);

    CHK_STATUS(getLocalhostIpAddresses(pIceAgent->localNetworkInterfaces,
                                       &pIceAgent->localNetworkInterfaceCount,
                                       pIceAgent->kvsRtcConfiguration.iceSetInterfaceFilterFunc,
                                       pIceAgent->kvsRtcConfiguration.filterCustomData));

    // skip gathering host candidate and srflx candidate if relay only
    if (pIceAgent->iceTransportPolicy != ICE_TRANSPORT_POLICY_RELAY) {
        CHK_STATUS(iceAgentInitHostCandidate(pIceAgent));
        CHK_STATUS(iceAgentInitSrflxCandidate(pIceAgent));
    }

    CHK_STATUS(iceAgentInitRelayCandidate(pIceAgent));

    // start listening for incoming data
    CHK_STATUS(connectionListenerStart(pIceAgent->pConnectionListener));

    pIceAgent->candidateGatheringEndTime = GETTIME() + pIceAgent->kvsRtcConfiguration.iceLocalCandidateGatheringTimeout;

    CHK_STATUS(timerQueueAddTimer(pIceAgent->timerQueueHandle,
                                  KVS_ICE_DEFAULT_TIMER_START_DELAY,
                                  KVS_ICE_GATHER_CANDIDATE_TIMER_POLLING_INTERVAL,
                                  iceAgentGatherCandidateTimerCallback,
                                  (UINT64) pIceAgent,
                                  &pIceAgent->iceCandidateGatheringTimerTask));

CleanUp:

    CHK_LOG_ERR(retStatus);

    if (STATUS_FAILED(retStatus)) {
        iceAgentFatalError(pIceAgent, retStatus);
    }

    return retStatus;
}

STATUS iceAgentSendPacket(PIceAgent pIceAgent, PBYTE pBuffer, UINT32 bufferLen)
{
    STATUS retStatus = STATUS_SUCCESS;
    BOOL locked = FALSE;

    SocketConnection socketConnection;
    KvsIpAddress destAddr;
    BOOL isRelay = FALSE;
    PTurnConnection pTurnConnection = NULL;

    CHK(pIceAgent != NULL && pBuffer != NULL, STATUS_NULL_ARG);
    CHK(bufferLen != 0, STATUS_INVALID_ARG);

    MUTEX_LOCK(pIceAgent->lock);
    locked = TRUE;

    CHK_WARN(pIceAgent->pDataSendingIceCandidatePair != NULL, retStatus, "No valid ice candidate pair available to send data");

    pIceAgent->pDataSendingIceCandidatePair->lastDataSentTime = GETTIME();

    // Construct context
    socketConnection = *pIceAgent->pDataSendingIceCandidatePair->local->pSocketConnection;
    destAddr = pIceAgent->pDataSendingIceCandidatePair->remote->ipAddress;
    isRelay = IS_CANN_PAIR_SENDING_FROM_RELAYED(pIceAgent->pDataSendingIceCandidatePair);

    if (isRelay) {
        CHK_ERR(pIceAgent->pDataSendingIceCandidatePair->local->pTurnConnectionTracker != NULL,
                STATUS_NULL_ARG, "Candidate is relay but pTurnConnectionTracker is NULL");
        pTurnConnection = pIceAgent->pDataSendingIceCandidatePair->local->pTurnConnectionTracker->pTurnConnection;
    }

    retStatus = iceUtilsSendData(pBuffer,
                                 bufferLen,
                                 &destAddr,
                                 &socketConnection,
                                 pTurnConnection,
                                 isRelay);

    if (STATUS_FAILED(retStatus)) {
        DLOGW("iceUtilsSendData failed with 0x%08x", retStatus);
        retStatus = STATUS_SUCCESS;
    }

CleanUp:

    if (locked) {
        MUTEX_UNLOCK(pIceAgent->lock);
    }

    return retStatus;
}

STATUS iceAgentPopulateSdpMediaDescriptionCandidates(PIceAgent pIceAgent, PSdpMediaDescription pSdpMediaDescription, UINT32 attrBufferLen, PUINT32 pIndex)
{
    STATUS retStatus = STATUS_SUCCESS;
    UINT64 data;
    PDoubleListNode pCurNode = NULL;
    BOOL locked = FALSE;
    UINT32 attrIndex;

    CHK(pIceAgent != NULL && pSdpMediaDescription != NULL && pIndex != NULL, STATUS_NULL_ARG);

    attrIndex = *pIndex;

    MUTEX_LOCK(pIceAgent->lock);
    locked = TRUE;

    CHK_STATUS(doubleListGetHeadNode(pIceAgent->localCandidates, &pCurNode));
    while (pCurNode != NULL) {
        CHK_STATUS(doubleListGetNodeData(pCurNode, &data));
        pCurNode = pCurNode->pNext;

        STRCPY(pSdpMediaDescription->sdpAttributes[attrIndex].attributeName, "candidate");
        CHK_STATUS(iceCandidateSerialize((PIceCandidate) data, pSdpMediaDescription->sdpAttributes[attrIndex].attributeValue, &attrBufferLen));
        attrIndex++;
    }

    *pIndex = attrIndex;

CleanUp:

    if (locked) {
        MUTEX_UNLOCK(pIceAgent->lock);
    }

    return retStatus;
}

STATUS iceAgentShutdown(PIceAgent pIceAgent)
{
    STATUS retStatus = STATUS_SUCCESS;
    BOOL locked = FALSE;
    PDoubleListNode pCurNode = NULL;
    PIceCandidate pLocalCandidate = NULL;
    UINT32 i;

    CHK(pIceAgent != NULL, STATUS_NULL_ARG);

    if (pIceAgent->iceAgentStateTimerTask != UINT32_MAX) {
        CHK_STATUS(timerQueueCancelTimer(pIceAgent->timerQueueHandle, pIceAgent->iceAgentStateTimerTask, (UINT64) pIceAgent));
        pIceAgent->iceAgentStateTimerTask = UINT32_MAX;
    }

    if (pIceAgent->keepAliveTimerTask != UINT32_MAX) {
        CHK_STATUS(timerQueueCancelTimer(pIceAgent->timerQueueHandle, pIceAgent->keepAliveTimerTask, (UINT64) pIceAgent));
        pIceAgent->keepAliveTimerTask = UINT32_MAX;
    }

    MUTEX_LOCK(pIceAgent->lock);
    locked = TRUE;

    CHK_STATUS(doubleListGetHeadNode(pIceAgent->localCandidates, &pCurNode));
    while (pCurNode != NULL) {
        pLocalCandidate = (PIceCandidate) pCurNode->data;
        pCurNode = pCurNode->pNext;

        if (pLocalCandidate->iceCandidateType != ICE_CANDIDATE_TYPE_RELAYED) {
            CHK_STATUS(connectionListenerRemoveConnection(pIceAgent->pConnectionListener, pLocalCandidate->pSocketConnection));
        }
    }

    MUTEX_UNLOCK(pIceAgent->lock);
    locked = FALSE;

    /* free turn allocation. */
    for(i = 0; i < pIceAgent->turnConnectionTrackerCount; ++i) {
        if (pIceAgent->turnConnectionTrackers[i].freeTurnConnectionTimerId != UINT32_MAX) {
            CHK_STATUS(timerQueueCancelTimer(pIceAgent->timerQueueHandle,
                                             pIceAgent->turnConnectionTrackers[i].freeTurnConnectionTimerId,
                                             (UINT64) &pIceAgent->turnConnectionTrackers[i]));
            pIceAgent->turnConnectionTrackers[i].freeTurnConnectionTimerId = UINT32_MAX;
        }

        /* could have already been freed by iceAgentFreeTurnConnectionTimerCallback() */
        if (pIceAgent->turnConnectionTrackers[i].pTurnConnection != NULL) {
            CHK_STATUS(turnConnectionShutdown(pIceAgent->turnConnectionTrackers[i].pTurnConnection, KVS_ICE_TURN_CONNECTION_SHUTDOWN_TIMEOUT));
            CHK_LOG_ERR(freeTurnConnection(&pIceAgent->turnConnectionTrackers[i].pTurnConnection));
        }
    }
    pIceAgent->turnConnectionTrackerCount = 0;

    /* remove connections last because still need to send data to deallocate turn */
    if (pIceAgent->pConnectionListener != NULL) {
        CHK_STATUS(connectionListenerRemoveAllConnection(pIceAgent->pConnectionListener));
    }

CleanUp:

    CHK_LOG_ERR(retStatus);

    if (locked) {
        MUTEX_UNLOCK(pIceAgent->lock);
    }

    return retStatus;
}

//////////////////////////////////////////////
// internal functions
//////////////////////////////////////////////

STATUS findCandidateWithIp(PKvsIpAddress pIpAddress, PDoubleList pCandidateList, PIceCandidate *ppIceCandidate)
{
    ENTERS();

    STATUS retStatus = STATUS_SUCCESS;
    PDoubleListNode pCurNode = NULL;
    UINT64 data;
    PIceCandidate pIceCandidate = NULL, pTargetIceCandidate = NULL;
    UINT32 addrLen;

    CHK(pIpAddress != NULL && pCandidateList != NULL && ppIceCandidate != NULL, STATUS_NULL_ARG);

    CHK_STATUS(doubleListGetHeadNode(pCandidateList, &pCurNode));
    while (pCurNode != NULL && pTargetIceCandidate == NULL) {
        CHK_STATUS(doubleListGetNodeData(pCurNode, &data));
        pIceCandidate = (PIceCandidate) data;
        pCurNode = pCurNode->pNext;

        addrLen = IS_IPV4_ADDR(pIpAddress) ? IPV4_ADDRESS_LENGTH : IPV6_ADDRESS_LENGTH;
        if (pIpAddress->family == pIceCandidate->ipAddress.family &&
            MEMCMP(pIceCandidate->ipAddress.address, pIpAddress->address, addrLen) == 0 &&
            pIpAddress->port == pIceCandidate->ipAddress.port) {
            pTargetIceCandidate = pIceCandidate;
        }
    }

CleanUp:

    if (ppIceCandidate != NULL) {
        *ppIceCandidate = pTargetIceCandidate;
    }

    LEAVES();
    return retStatus;
}

STATUS findCandidateWithSocketConnection(PSocketConnection pSocketConnection, PDoubleList pCandidateList,
                                         PIceCandidate *ppIceCandidate)
{
    ENTERS();

    STATUS retStatus = STATUS_SUCCESS;
    PDoubleListNode pCurNode = NULL;
    UINT64 data;
    PIceCandidate pIceCandidate = NULL, pTargetIceCandidate = NULL;

    CHK(pCandidateList != NULL && ppIceCandidate != NULL && pSocketConnection != NULL, STATUS_NULL_ARG);

    CHK_STATUS(doubleListGetHeadNode(pCandidateList, &pCurNode));
    while (pCurNode != NULL && pTargetIceCandidate == NULL) {
        CHK_STATUS(doubleListGetNodeData(pCurNode, &data));
        pIceCandidate = (PIceCandidate) data;
        pCurNode = pCurNode->pNext;

        if (pIceCandidate->pSocketConnection == pSocketConnection) {
            pTargetIceCandidate = pIceCandidate;
        }
    }

CleanUp:

    if (ppIceCandidate != NULL) {
        *ppIceCandidate = pTargetIceCandidate;
    }

    LEAVES();
    return retStatus;
}

/*
 * Need to acquire pIceAgent->lock first
 */
STATUS createIceCandidatePairs(PIceAgent pIceAgent, PIceCandidate pIceCandidate, BOOL isRemoteCandidate)
{
    ENTERS();

    STATUS retStatus = STATUS_SUCCESS;
    UINT64 data;
    PDoubleListNode pCurNode = NULL;
    PDoubleList pDoubleList = NULL;
    PIceCandidatePair pIceCandidatePair = NULL;
    BOOL freeObjOnFailure = TRUE;
    PIceCandidate pCurrentIceCandidate = NULL;

    CHK(pIceAgent != NULL && pIceCandidate != NULL, STATUS_NULL_ARG);
    CHK_WARN(pIceCandidate->state == ICE_CANDIDATE_STATE_VALID, retStatus,
             "New ice candidate need to be valid to form pairs");

    // if pIceCandidate is a remote candidate, then form pairs with every single valid local candidate. Otherwize,
    // form pairs with every single valid remote candidate
    pDoubleList = isRemoteCandidate ? pIceAgent->localCandidates : pIceAgent->remoteCandidates;

    CHK_STATUS(doubleListGetHeadNode(pDoubleList, &pCurNode));
    while (pCurNode != NULL) {
        CHK_STATUS(doubleListGetNodeData(pCurNode, &data));
        pCurrentIceCandidate = (PIceCandidate) data;
        pCurNode = pCurNode->pNext;

        if (pCurrentIceCandidate->state == ICE_CANDIDATE_STATE_VALID) {
            pIceCandidatePair = (PIceCandidatePair) MEMCALLOC(1, SIZEOF(IceCandidatePair));
            CHK(pIceCandidatePair != NULL, STATUS_NOT_ENOUGH_MEMORY);

            if (isRemoteCandidate) {
                pIceCandidatePair->local = (PIceCandidate) data;
                pIceCandidatePair->remote = pIceCandidate;
            } else {
                pIceCandidatePair->local = pIceCandidate;
                pIceCandidatePair->remote = (PIceCandidate) data;
            }
            pIceCandidatePair->nominated = FALSE;

            // ensure the new pair will go through connectivity check as soon as possible
            pIceCandidatePair->state = ICE_CANDIDATE_PAIR_STATE_WAITING;

            CHK_STATUS(createTransactionIdStore(DEFAULT_MAX_STORED_TRANSACTION_ID_COUNT,
                                                &pIceCandidatePair->pTransactionIdStore));

            pIceCandidatePair->lastDataSentTime = 0;
            pIceCandidatePair->priority = computeCandidatePairPriority(
                    pIceCandidatePair,
                    pIceAgent->isControlling);
            CHK_STATUS(insertIceCandidatePair(pIceAgent->iceCandidatePairs, pIceCandidatePair));
            freeObjOnFailure = FALSE;
        }
    }

CleanUp:

    CHK_LOG_ERR(retStatus);

    if (STATUS_FAILED(retStatus) && freeObjOnFailure) {
        freeIceCandidatePair(&pIceCandidatePair);
    }

    LEAVES();
    return retStatus;
}

STATUS freeIceCandidatePair(PIceCandidatePair* ppIceCandidatePair)
{
    ENTERS();
    STATUS retStatus = STATUS_SUCCESS;
    PIceCandidatePair pIceCandidatePair = NULL;

    CHK(ppIceCandidatePair != NULL, STATUS_NULL_ARG);
    // free is idempotent
    CHK(*ppIceCandidatePair != NULL, retStatus);
    pIceCandidatePair = *ppIceCandidatePair;

    CHK_LOG_ERR(freeTransactionIdStore(&pIceCandidatePair->pTransactionIdStore));
    SAFE_MEMFREE(pIceCandidatePair);

CleanUp:

    LEAVES();
    return retStatus;
}

STATUS insertIceCandidatePair(PDoubleList iceCandidatePairs, PIceCandidatePair pIceCandidatePair)
{
    ENTERS();
    STATUS retStatus = STATUS_SUCCESS;
    PDoubleListNode pCurNode = NULL;
    PIceCandidatePair pCurIceCandidatePair = NULL;

    CHK(iceCandidatePairs != NULL && pIceCandidatePair != NULL, STATUS_NULL_ARG);

    CHK_STATUS(doubleListGetHeadNode(iceCandidatePairs, &pCurNode));

    while(pCurNode != NULL) {
        pCurIceCandidatePair = (PIceCandidatePair) pCurNode->data;

        // insert new candidate pair ordered by priority from max to min.
        if (pCurIceCandidatePair->priority <= pIceCandidatePair->priority) {
            break;
        }
        pCurNode = pCurNode->pNext;
    }

    if (pCurNode != NULL) {
        CHK_STATUS(doubleListInsertItemBefore(iceCandidatePairs, pCurNode, (UINT64) pIceCandidatePair));
    } else {
        CHK_STATUS(doubleListInsertItemTail(iceCandidatePairs, (UINT64) pIceCandidatePair));
    }

CleanUp:

    CHK_LOG_ERR(retStatus);

    LEAVES();
    return retStatus;
}

STATUS findIceCandidatePairWithLocalSocketConnectionAndRemoteAddr(PIceAgent pIceAgent, PSocketConnection pSocketConnection,
                                                                  PKvsIpAddress pRemoteAddr, BOOL checkPort,
                                                                  PIceCandidatePair* ppIceCandidatePair)
{
    ENTERS();

    STATUS retStatus = STATUS_SUCCESS;
    UINT32 addrLen;
    PIceCandidatePair pTargetIceCandidatePair = NULL, pIceCandidatePair = NULL;
    PDoubleListNode pCurNode = NULL;

    CHK(pIceAgent != NULL && ppIceCandidatePair != NULL && pSocketConnection != NULL, STATUS_NULL_ARG);

    addrLen = IS_IPV4_ADDR(pRemoteAddr) ? IPV4_ADDRESS_LENGTH : IPV6_ADDRESS_LENGTH;

    CHK_STATUS(doubleListGetHeadNode(pIceAgent->iceCandidatePairs, &pCurNode));
    while (pCurNode != NULL && pTargetIceCandidatePair == NULL) {
        pIceCandidatePair = (PIceCandidatePair) pCurNode->data;
        pCurNode = pCurNode->pNext;

        if (pIceCandidatePair->state != ICE_CANDIDATE_PAIR_STATE_FAILED &&
            pIceCandidatePair->local->pSocketConnection == pSocketConnection &&
            pIceCandidatePair->remote->ipAddress.family == pRemoteAddr->family &&
            MEMCMP(pIceCandidatePair->remote->ipAddress.address, pRemoteAddr->address, addrLen) == 0 &&
            (!checkPort || pIceCandidatePair->remote->ipAddress.port == pRemoteAddr->port)) {
            pTargetIceCandidatePair = pIceCandidatePair;
        }
    }

CleanUp:

    if (ppIceCandidatePair != NULL) {
        *ppIceCandidatePair = pTargetIceCandidatePair;
    }

    LEAVES();
    return retStatus;
}

STATUS pruneUnconnectedIceCandidatePair(PIceAgent pIceAgent)
{
    ENTERS();

    STATUS retStatus = STATUS_SUCCESS;
    PDoubleListNode pCurNode = NULL, pNextNode = NULL;
    PIceCandidatePair pIceCandidatePair = NULL;

    CHK(pIceAgent != NULL, STATUS_NULL_ARG);

    CHK_STATUS(doubleListGetHeadNode(pIceAgent->iceCandidatePairs, &pCurNode));
    while (pCurNode != NULL) {
        pIceCandidatePair = (PIceCandidatePair) pCurNode->data;

        if (pIceCandidatePair->state != ICE_CANDIDATE_PAIR_STATE_SUCCEEDED) {
            // backup next node as we will lose that after deleting pCurNode.
            pNextNode = pCurNode->pNext;
            CHK_STATUS(freeIceCandidatePair(&pIceCandidatePair));
            CHK_STATUS(doubleListDeleteNode(pIceAgent->iceCandidatePairs, pCurNode));
            pCurNode = pNextNode;
        } else {
            pCurNode = pCurNode->pNext;
        }
    }

CleanUp:

    CHK_LOG_ERR(retStatus);

    LEAVES();
    return retStatus;
}

STATUS iceCandidatePairCheckConnection(PStunPacket pStunBindingRequest, PIceAgent pIceAgent, PIceCandidatePair pIceCandidatePair)
{
    STATUS retStatus = STATUS_SUCCESS;
    PStunAttributePriority pStunAttributePriority = NULL;

    CHK(pStunBindingRequest != NULL && pIceAgent != NULL && pIceCandidatePair != NULL, STATUS_NULL_ARG);

    CHK_STATUS(getStunAttribute(pStunBindingRequest, STUN_ATTRIBUTE_TYPE_PRIORITY, (PStunAttributeHeader *) &pStunAttributePriority));
    CHK(pStunAttributePriority != NULL, STATUS_INVALID_ARG);

    // update priority and transaction id
    pStunAttributePriority->priority = pIceCandidatePair->local->priority;
    CHK_STATUS(iceUtilsGenerateTransactionId(pStunBindingRequest->header.transactionId,
                                     ARRAY_SIZE(pStunBindingRequest->header.transactionId)));

    CHK(pIceCandidatePair->pTransactionIdStore != NULL, STATUS_INVALID_OPERATION);
    transactionIdStoreInsert(pIceCandidatePair->pTransactionIdStore, pStunBindingRequest->header.transactionId);

    CHK_STATUS(iceAgentSendStunPacket(pStunBindingRequest,
                                      (PBYTE) pIceAgent->remotePassword,
                                      (UINT32) STRLEN(pIceAgent->remotePassword) * SIZEOF(CHAR),
                                      pIceAgent,
                                      pIceCandidatePair->local,
                                      &pIceCandidatePair->remote->ipAddress));

CleanUp:

    CHK_LOG_ERR(retStatus);

    return retStatus;
}

STATUS iceAgentSendStunPacket(PStunPacket pStunPacket, PBYTE password, UINT32 passwordLen, PIceAgent pIceAgent,
                              PIceCandidate pLocalCandidate, PKvsIpAddress pDestAddr)
{
    STATUS retStatus = STATUS_SUCCESS;
    UINT32 stunPacketSize = STUN_PACKET_ALLOCATION_SIZE;
    BYTE stunPacketBuffer[STUN_PACKET_ALLOCATION_SIZE];
    PIceCandidatePair pIceCandidatePair = NULL;
    PTurnConnection pTurnConnection = NULL;

    // Assuming holding pIceAgent->lock

    CHK(pStunPacket != NULL && pIceAgent != NULL && pLocalCandidate != NULL && pDestAddr != NULL, STATUS_NULL_ARG);

    CHK_STATUS(iceUtilsPackageStunPacket(pStunPacket, password, passwordLen, stunPacketBuffer, &stunPacketSize));
    pTurnConnection = pLocalCandidate->pTurnConnectionTracker != NULL ? pLocalCandidate->pTurnConnectionTracker->pTurnConnection : NULL;
    retStatus = iceUtilsSendData((PBYTE) stunPacketBuffer,
                                 stunPacketSize,
                                 pDestAddr,
                                 pLocalCandidate->pSocketConnection,
                                 pTurnConnection,
                                 pLocalCandidate->iceCandidateType == ICE_CANDIDATE_TYPE_RELAYED);

    if (STATUS_FAILED(retStatus)) {
        DLOGW("iceUtilsSendData failed with 0x%08x. Mark candidate pair as failed.", retStatus);
        retStatus = STATUS_SUCCESS;

        /* Update iceCandidatePair state to failed.
         * pIceCandidatePair could no longer exist. */
        CHK_STATUS(findIceCandidatePairWithLocalSocketConnectionAndRemoteAddr(pIceAgent, pLocalCandidate->pSocketConnection, pDestAddr, TRUE, &pIceCandidatePair));

        if (pIceCandidatePair != NULL) {
            pIceCandidatePair->state = ICE_CANDIDATE_PAIR_STATE_FAILED;
        }
    }

CleanUp:

    CHK_LOG_ERR(retStatus);

    return retStatus;
}

/**
 * timer queue callbacks are interlocked by time queue lock.
 *
 * @param timerId - timer queue task id
 * @param currentTime
 * @param customData - custom data passed to timer queue when task was added
 * @return
 */
STATUS iceAgentStateTransitionTimerCallback(UINT32 timerId, UINT64 currentTime, UINT64 customData)
{
    UNUSED_PARAM(timerId);
    UNUSED_PARAM(currentTime);
    STATUS retStatus = STATUS_SUCCESS;
    PIceAgent pIceAgent = (PIceAgent) customData;

    CHK(pIceAgent != NULL, STATUS_NULL_ARG);

    // Do not acquire lock because stepIceAgentStateMachine acquires lock.
    // Drive the state machine
    CHK_STATUS(stepIceAgentStateMachine(pIceAgent));

CleanUp:

    CHK_LOG_ERR(retStatus);

    if (STATUS_FAILED(retStatus)) {
        iceAgentFatalError(pIceAgent, retStatus);
    }

    return retStatus;
}

STATUS iceAgentSendSrflxCandidateRequest(PIceAgent pIceAgent)
{
    STATUS retStatus = STATUS_SUCCESS;
    PDoubleListNode pCurNode = NULL;
    UINT64 data;
    PIceCandidate pCandidate = NULL;
    PIceServer pIceServer = NULL;
    PStunPacket pBindingRequest = NULL;

    CHK(pIceAgent != NULL, STATUS_NULL_ARG);

    // Assume holding pIceAgent->lock

    /* Can't reuse pIceAgent->pBindingRequest because candidate gathering could be running in parallel with
     * connection check. */
    CHK_STATUS(createStunPacket(STUN_PACKET_TYPE_BINDING_REQUEST, NULL, &pBindingRequest));

    CHK_STATUS(doubleListGetHeadNode(pIceAgent->localCandidates, &pCurNode));
    while (pCurNode != NULL) {
        CHK_STATUS(doubleListGetNodeData(pCurNode, &data));
        pCurNode = pCurNode->pNext;
        pCandidate = (PIceCandidate) data;

        if (pCandidate->state == ICE_CANDIDATE_STATE_NEW){
            switch(pCandidate->iceCandidateType) {
                case ICE_CANDIDATE_TYPE_SERVER_REFLEXIVE:
                    pIceServer = &(pIceAgent->iceServers[pCandidate->iceServerIndex]);
                    transactionIdStoreInsert(pIceAgent->pStunBindingRequestTransactionIdStore,
                                             pBindingRequest->header.transactionId);
                    retStatus =  iceUtilsSendStunPacket(pBindingRequest,
                                                        NULL,
                                                        0,
                                                        &pIceServer->ipAddress,
                                                        pCandidate->pSocketConnection,
                                                        NULL,
                                                        FALSE);
                    if (STATUS_FAILED(retStatus)) {
                        DLOGD("iceUtilsSendStunPacket failed with 0x%08x", retStatus);
                        retStatus = STATUS_SUCCESS;
                    }

                    break;

                default:
                    break;
            }
        }
    }

CleanUp:

    CHK_LOG_ERR(retStatus);

    if (pBindingRequest != NULL) {
        freeStunPacket(&pBindingRequest);
    }

    if (STATUS_FAILED(retStatus)) {
        iceAgentFatalError(pIceAgent, retStatus);
    }

    return retStatus;
}

STATUS iceAgentCheckCandidatePairConnection(PIceAgent pIceAgent)
{
    STATUS retStatus = STATUS_SUCCESS;
    BOOL triggeredCheckQueueEmpty;
    UINT64 data;
    PIceCandidatePair pIceCandidatePair = NULL;
    PDoubleListNode pCurNode = NULL;

    CHK(pIceAgent != NULL, STATUS_NULL_ARG);

    // Assuming pIceAgent->candidatePairs is sorted by priority
    // Assume holding pIceAgent->lock

    CHK_STATUS(stackQueueIsEmpty(pIceAgent->triggeredCheckQueue, &triggeredCheckQueueEmpty));
    if (!triggeredCheckQueueEmpty) {
        // if triggeredCheckQueue is not empty, check its candidate pair first
        stackQueueDequeue(pIceAgent->triggeredCheckQueue, &data);
        pIceCandidatePair = (PIceCandidatePair) data;

        CHK_STATUS(iceCandidatePairCheckConnection(pIceAgent->pBindingRequest, pIceAgent, pIceCandidatePair));
    } else {

        CHK_STATUS(doubleListGetHeadNode(pIceAgent->iceCandidatePairs, &pCurNode));
        while (pCurNode != NULL) {
            pIceCandidatePair = (PIceCandidatePair) pCurNode->data;
            pCurNode = pCurNode->pNext;

            switch (pIceCandidatePair->state) {
                case ICE_CANDIDATE_PAIR_STATE_WAITING:
                    pIceCandidatePair->state = ICE_CANDIDATE_PAIR_STATE_IN_PROGRESS;
                    // NOTE: Explicit fall-through
                case ICE_CANDIDATE_PAIR_STATE_IN_PROGRESS:
                    CHK_STATUS(iceCandidatePairCheckConnection(pIceAgent->pBindingRequest, pIceAgent, pIceCandidatePair));
                    break;
                default:
                    break;
            }
        }
    }

CleanUp:

    CHK_LOG_ERR(retStatus);

    if (STATUS_FAILED(retStatus)) {
        iceAgentFatalError(pIceAgent, retStatus);
    }

    return retStatus;
}

STATUS iceAgentSendKeepAliveTimerCallback(UINT32 timerId, UINT64 currentTime, UINT64 customData)
{
    UNUSED_PARAM(timerId);
    STATUS retStatus = STATUS_SUCCESS;
    PIceAgent pIceAgent = (PIceAgent) customData;
    BOOL locked = FALSE;
    PIceCandidatePair pIceCandidatePair = NULL;
    PDoubleListNode pCurNode = NULL;

    CHK(pIceAgent != NULL, STATUS_NULL_ARG);

    MUTEX_LOCK(pIceAgent->lock);
    locked = TRUE;

    CHK_STATUS(doubleListGetHeadNode(pIceAgent->iceCandidatePairs, &pCurNode));
    while (pCurNode != NULL) {
        pIceCandidatePair = (PIceCandidatePair) pCurNode->data;
        pCurNode = pCurNode->pNext;

        if (pIceCandidatePair->state == ICE_CANDIDATE_PAIR_STATE_SUCCEEDED) {

            pIceCandidatePair->lastDataSentTime = currentTime;
            DLOGV("send keep alive");
            CHK_STATUS(iceAgentSendStunPacket(pIceAgent->pBindingIndication, NULL, 0, pIceAgent,
                                              pIceCandidatePair->local, &pIceCandidatePair->remote->ipAddress));
        }
    }

CleanUp:

    CHK_LOG_ERR(retStatus);

    if (STATUS_FAILED(retStatus)) {
        iceAgentFatalError(pIceAgent, retStatus);
    }

    if (locked) {
        MUTEX_UNLOCK(pIceAgent->lock);
    }

    return retStatus;
}

STATUS iceAgentFreeTurnConnectionTimerCallback(UINT32 timerId, UINT64 currentTime, UINT64 customData)
{
    UNUSED_PARAM(timerId);
    STATUS retStatus = STATUS_SUCCESS;
    PTurnConnectionTracker pTurnConnectionTracker = (PTurnConnectionTracker) customData;

    CHK(pTurnConnectionTracker != NULL, STATUS_NULL_ARG);

    if (turnConnectionIsShutdownComplete(pTurnConnectionTracker->pTurnConnection) ||
            currentTime > pTurnConnectionTracker->freeTurnConnectionEndTime) {
        freeTurnConnection(&pTurnConnectionTracker->pTurnConnection);
        pTurnConnectionTracker->pTurnConnection = NULL;
        pTurnConnectionTracker->freeTurnConnectionTimerId = UINT32_MAX;
        retStatus = STATUS_TIMER_QUEUE_STOP_SCHEDULING;
    }

CleanUp:

    return retStatus;
}

STATUS iceAgentGatherCandidateTimerCallback(UINT32 timerId, UINT64 currentTime, UINT64 customData)
{
    UNUSED_PARAM(timerId);
    STATUS retStatus = STATUS_SUCCESS;
    PIceAgent pIceAgent = (PIceAgent) customData;
    BOOL locked = FALSE, stopScheduling = FALSE;
    PDoubleListNode pCurNode = NULL;
    UINT64 data;
    PIceCandidate pIceCandidate = NULL;
    UINT32 pendingSrflxCandidateCount = 0, pendingCandidateCount = 0;
    PKvsIpAddress pRelayAddress = NULL;

    CHK(pIceAgent != NULL, STATUS_NULL_ARG);

    MUTEX_LOCK(pIceAgent->lock);
    locked = TRUE;

    CHK_STATUS(doubleListGetHeadNode(pIceAgent->localCandidates, &pCurNode));
    while (pCurNode != NULL) {
        CHK_STATUS(doubleListGetNodeData(pCurNode, &data));
        pCurNode = pCurNode->pNext;
        pIceCandidate = (PIceCandidate) data;

        if (pIceCandidate->state == ICE_CANDIDATE_STATE_NEW){
            pendingCandidateCount++;
            if (pIceCandidate->iceCandidateType == ICE_CANDIDATE_TYPE_SERVER_REFLEXIVE) {
                pendingSrflxCandidateCount++;
            } else if (pIceCandidate->iceCandidateType == ICE_CANDIDATE_TYPE_RELAYED &&
                       pIceCandidate->pTurnConnectionTracker != NULL &&
                       (pRelayAddress = turnConnectionGetRelayAddress(pIceCandidate->pTurnConnectionTracker->pTurnConnection)) != NULL) {
                /* Check if any relay address has been obtained. */
                CHK_STATUS(updateCandidateAddress(pIceCandidate, pRelayAddress));
                CHK_STATUS(iceAgentReportNewLocalCandidate(pIceAgent, pIceCandidate));
                CHK_STATUS(createIceCandidatePairs(pIceAgent, pIceCandidate, FALSE));
            }
        }
    }

    // keep sending binding request if there is still pending srflx candidate
    if (pendingSrflxCandidateCount > 0) {
        CHK_STATUS(iceAgentSendSrflxCandidateRequest(pIceAgent));
    }

    // stop scheduling if there is no more pending candidate or if timeout is reached.
    if (pendingCandidateCount == 0 || currentTime >= pIceAgent->candidateGatheringEndTime) {
        DLOGD("Candidate gathering completed.");
        stopScheduling = TRUE;
        pIceAgent->iceCandidateGatheringTimerTask = UINT32_MAX;
        ATOMIC_STORE_BOOL(&pIceAgent->candidateGatheringFinished, TRUE);
        // notify that candidate gathering is finished.
        if (pIceAgent->iceAgentCallbacks.newLocalCandidateFn != NULL) {
            pIceAgent->iceAgentCallbacks.newLocalCandidateFn(pIceAgent->iceAgentCallbacks.customData, NULL);
        }
    }

CleanUp:

    CHK_LOG_ERR(retStatus);

    if (STATUS_FAILED(retStatus)) {
        iceAgentFatalError(pIceAgent, retStatus);
    }
    if (stopScheduling) {
        retStatus = STATUS_TIMER_QUEUE_STOP_SCHEDULING;
    }

    if (locked) {
        MUTEX_UNLOCK(pIceAgent->lock);
    }

    return retStatus;
}

STATUS iceAgentSendCandidateNomination(PIceAgent pIceAgent)
{
    STATUS retStatus = STATUS_SUCCESS;
    PDoubleListNode pCurNode = NULL;
    PIceCandidatePair pIceCandidatePair = NULL;

    CHK(pIceAgent != NULL, STATUS_NULL_ARG);
    // do nothing if not controlling
    CHK(pIceAgent->isControlling, retStatus);

    // Assume holding pIceAgent->lock

    // send packet with USE_CANDIDATE flag if is controlling
    CHK_STATUS(doubleListGetHeadNode(pIceAgent->iceCandidatePairs, &pCurNode));
    while (pCurNode != NULL) {
        pIceCandidatePair = (PIceCandidatePair) pCurNode->data;
        pCurNode = pCurNode->pNext;

        if (pIceCandidatePair->nominated) {
            CHK_STATUS(iceCandidatePairCheckConnection(pIceAgent->pBindingRequest, pIceAgent, pIceCandidatePair));
        }
    }

CleanUp:

    CHK_LOG_ERR(retStatus);

    if (STATUS_FAILED(retStatus)) {
        iceAgentFatalError(pIceAgent, retStatus);
    }

    return retStatus;
}

STATUS iceAgentInitSrflxCandidate(PIceAgent pIceAgent)
{
    STATUS retStatus = STATUS_SUCCESS;
    PDoubleListNode pCurNode = NULL;
    UINT64 data;
    PIceCandidate pCandidate = NULL, pNewCandidate = NULL;
    UINT32 j;
    BOOL locked = FALSE;

    CHK(pIceAgent != NULL, STATUS_NULL_ARG);

    /* There should be no other thread mutating localCandidates at this time, so safe to read without lock. */
    CHK_STATUS(doubleListGetHeadNode(pIceAgent->localCandidates, &pCurNode));
    while (pCurNode != NULL) {
        CHK_STATUS(doubleListGetNodeData(pCurNode, &data));
        pCurNode = pCurNode->pNext;
        pCandidate = (PIceCandidate) data;

        if (pCandidate->iceCandidateType == ICE_CANDIDATE_TYPE_HOST) {
            for (j = 0; j < pIceAgent->iceServersCount; j++) {
                if (!pIceAgent->iceServers[j].isTurn) {
                    CHK((pNewCandidate = (PIceCandidate) MEMCALLOC(1, SIZEOF(IceCandidate))) != NULL,
                        STATUS_NOT_ENOUGH_MEMORY);

                    // copy over host candidate's address to open up a new socket at that address.
                    pNewCandidate->ipAddress = pCandidate->ipAddress;
                    // open up a new socket at host candidate's ip address for server reflex candidate.
                    // the new port will be stored in pNewCandidate->ipAddress.port. And the Ip address will later be updated
                    // with the correct ip address once the STUN response is received.
                    CHK_STATUS(createSocketConnection(&pNewCandidate->ipAddress, NULL, KVS_SOCKET_PROTOCOL_UDP,
                                                      (UINT64) pIceAgent, incomingDataHandler, pIceAgent->kvsRtcConfiguration.sendBufSize,
                                                      &pNewCandidate->pSocketConnection));
                    ATOMIC_STORE_BOOL(&pNewCandidate->pSocketConnection->receiveData, TRUE);
                    // connectionListener will free the pSocketConnection at the end.
                    CHK_STATUS(connectionListenerAddConnection(pIceAgent->pConnectionListener,
                                                               pNewCandidate->pSocketConnection));
                    pNewCandidate->iceCandidateType = ICE_CANDIDATE_TYPE_SERVER_REFLEXIVE;
                    pNewCandidate->state = ICE_CANDIDATE_STATE_NEW;
                    pNewCandidate->iceServerIndex = j;
                    pNewCandidate->foundation = pIceAgent->foundationCounter++; // we dont generate candidates that have the same foundation.
                    pNewCandidate->priority = computeCandidatePriority(pNewCandidate);

                    /* There could be another thread calling iceAgentAddRemoteCandidate which triggers createIceCandidatePairs.
                     * createIceCandidatePairs will read through localCandidates, since we are mutating localCandidates here,
                     * need to acquire lock. */
                    MUTEX_LOCK(pIceAgent->lock);
                    locked = TRUE;

                    CHK_STATUS(doubleListInsertItemHead(pIceAgent->localCandidates, (UINT64) pNewCandidate));

                    MUTEX_UNLOCK(pIceAgent->lock);
                    locked = FALSE;

                    pNewCandidate = NULL;
                }
            }
        }
    }

CleanUp:

    CHK_LOG_ERR(retStatus);

    if (locked) {
        MUTEX_UNLOCK(pIceAgent->lock);
    }

    if (pNewCandidate != NULL) {
        SAFE_MEMFREE(pNewCandidate);
    }

    if (STATUS_FAILED(retStatus)) {
        iceAgentFatalError(pIceAgent, retStatus);
    }

    return retStatus;
}

STATUS iceAgentInitRelayCandidate(PIceAgent pIceAgent)
{
    STATUS retStatus = STATUS_SUCCESS;
    PDoubleListNode pCurNode = NULL;
    UINT64 data;
    PIceCandidate pNewCandidate = NULL, pCandidate = NULL;
    UINT32 j, i;
    PKvsIpAddress pSocketAddrForTurn = NULL;
    BOOL locked = FALSE;
    CHAR ipAddrStr[KVS_IP_ADDRESS_STRING_BUFFER_LEN];
    PTurnConnectionTracker pCurrentTurnConnectionTracker = NULL;

    CHK(pIceAgent != NULL, STATUS_NULL_ARG);
    for (j = 0; j < pIceAgent->iceServersCount; j++) {
        if (pIceAgent->iceServers[j].isTurn) {
            pSocketAddrForTurn = NULL;
            // if an VPN interface (isPointToPoint is TRUE) is found, use that instead
            for(i = 0; i < pIceAgent->localNetworkInterfaceCount; ++i) {
                if(pIceAgent->localNetworkInterfaces[i].family == pIceAgent->iceServers[j].ipAddress.family &&
                   (pSocketAddrForTurn == NULL || pIceAgent->localNetworkInterfaces[i].isPointToPoint)) {
                    pSocketAddrForTurn = &pIceAgent->localNetworkInterfaces[i];
                }
            }

            if (pSocketAddrForTurn == NULL) {
                getIpAddrStr(&pIceAgent->iceServers[j].ipAddress, ipAddrStr, SIZEOF(ipAddrStr));
                DLOGW("No suitable local interface found for turn server %s", ipAddrStr);

            } else {
                pCurrentTurnConnectionTracker = &pIceAgent->turnConnectionTrackers[pIceAgent->turnConnectionTrackerCount];
                CHK((pNewCandidate = (PIceCandidate) MEMCALLOC(1, SIZEOF(IceCandidate))) != NULL,
                    STATUS_NOT_ENOUGH_MEMORY);

                // copy over host candidate's address to open up a new socket at that address, because createSocketConnection
                // would store port info into pNewCandidate->ipAddress
                pNewCandidate->ipAddress = *pSocketAddrForTurn;
                // open up a new socket at host candidate's ip address for relay candidate.
                // the new port will be stored in pNewCandidate->ipAddress.port. And the Ip address will later be updated
                // with the correct ip address once the STUN response is received. Relay candidate's socket is managed
                // by TurnConnection struct.
                CHK_STATUS(createSocketConnection(&pNewCandidate->ipAddress, &pIceAgent->iceServers[j].ipAddress, KVS_ICE_DEFAULT_TURN_PROTOCOL,
                                                  (UINT64) pNewCandidate, incomingRelayedDataHandler, pIceAgent->kvsRtcConfiguration.sendBufSize,
                                                  &pNewCandidate->pSocketConnection));
                // connectionListener will free the pSocketConnection at the end.
                CHK_STATUS(connectionListenerAddConnection(pIceAgent->pConnectionListener,
                                                           pNewCandidate->pSocketConnection));

                pNewCandidate->iceCandidateType = ICE_CANDIDATE_TYPE_RELAYED;
                pNewCandidate->state = ICE_CANDIDATE_STATE_NEW;
                pNewCandidate->iceServerIndex = j;
                pNewCandidate->foundation = pIceAgent->foundationCounter++; // we dont generate candidates that have the same foundation.
                pNewCandidate->priority = computeCandidatePriority(pNewCandidate);

                CHK_STATUS(createTurnConnection(&pIceAgent->iceServers[j], pIceAgent->timerQueueHandle, TURN_CONNECTION_DATA_TRANSFER_MODE_SEND_INDIDATION,
                                                KVS_ICE_DEFAULT_TURN_PROTOCOL, NULL, pNewCandidate->pSocketConnection, pIceAgent->pConnectionListener,
                                                &pCurrentTurnConnectionTracker->pTurnConnection));
                pCurrentTurnConnectionTracker->pRelayCandidate = pNewCandidate;
                pCurrentTurnConnectionTracker->pIceAgent = pIceAgent;
                pCurrentTurnConnectionTracker->freeTurnConnectionTimerId = UINT32_MAX;
                pCurrentTurnConnectionTracker->freeTurnConnectionEndTime = INVALID_TIMESTAMP_VALUE;
                pNewCandidate->pTurnConnectionTracker = pCurrentTurnConnectionTracker;

                CHK_STATUS(doubleListInsertItemHead(pIceAgent->localCandidates, (UINT64) pNewCandidate));
                pNewCandidate = NULL;

                MUTEX_LOCK(pIceAgent->lock);
                locked = TRUE;

                /* add existing remote candidates to turn. Need to acquire lock because remoteCandidates can be mutated by
                 * iceAgentAddRemoteCandidate calls. */
                CHK_STATUS(doubleListGetHeadNode(pIceAgent->remoteCandidates, &pCurNode));
                while (pCurNode != NULL) {
                    CHK_STATUS(doubleListGetNodeData(pCurNode, &data));
                    pCurNode = pCurNode->pNext;
                    pCandidate = (PIceCandidate) data;

                    CHK_STATUS(turnConnectionAddPeer(pCurrentTurnConnectionTracker->pTurnConnection,
                                                     &pCandidate->ipAddress));
                }

                MUTEX_UNLOCK(pIceAgent->lock);
                locked = FALSE;

                CHK_STATUS(turnConnectionStart(pCurrentTurnConnectionTracker->pTurnConnection));
                pIceAgent->turnConnectionTrackerCount++;
            }
        }
    }

CleanUp:

    if (locked) {
        MUTEX_UNLOCK(pIceAgent->lock);
    }

    CHK_LOG_ERR(retStatus);

    SAFE_MEMFREE(pNewCandidate);

    if (STATUS_FAILED(retStatus)) {
        iceAgentFatalError(pIceAgent, retStatus);
    }

    return retStatus;
}

STATUS iceAgentCheckConnectionStateSetup(PIceAgent pIceAgent)
{
    STATUS retStatus = STATUS_SUCCESS;
    UINT32 iceCandidatePairCount = 0;
    PDoubleListNode pCurNode = NULL;
    PIceCandidatePair pIceCandidatePair = NULL;

    CHK(pIceAgent != NULL, STATUS_NULL_ARG);

    // Assume holding pIceAgent->lock
    CHK_STATUS(doubleListGetNodeCount(pIceAgent->iceCandidatePairs, &iceCandidatePairCount));

    DLOGD("ice candidate pair count %u", iceCandidatePairCount);

    // move all candidate pairs out of frozen state
    CHK_STATUS(doubleListGetHeadNode(pIceAgent->iceCandidatePairs, &pCurNode));
    while (pCurNode != NULL) {
        pIceCandidatePair = (PIceCandidatePair) pCurNode->data;
        pCurNode = pCurNode->pNext;

        pIceCandidatePair->state = ICE_CANDIDATE_PAIR_STATE_WAITING;
    }

    if (pIceAgent->pBindingRequest != NULL) {
        CHK_STATUS(freeStunPacket(&pIceAgent->pBindingRequest));
    }
    CHK_STATUS(createStunPacket(STUN_PACKET_TYPE_BINDING_REQUEST, NULL, &pIceAgent->pBindingRequest));
    CHK_STATUS(appendStunUsernameAttribute(pIceAgent->pBindingRequest, pIceAgent->combinedUserName));
    CHK_STATUS(appendStunPriorityAttribute(pIceAgent->pBindingRequest, 0));
    CHK_STATUS(appendStunIceControllAttribute(
            pIceAgent->pBindingRequest,
            pIceAgent->isControlling ? STUN_ATTRIBUTE_TYPE_ICE_CONTROLLING : STUN_ATTRIBUTE_TYPE_ICE_CONTROLLED,
            pIceAgent->tieBreaker));

    pIceAgent->stateEndTime = GETTIME() + pIceAgent->kvsRtcConfiguration.iceConnectionCheckTimeout;

CleanUp:

    CHK_LOG_ERR(retStatus);

    if (STATUS_FAILED(retStatus)) {
        iceAgentFatalError(pIceAgent, retStatus);
    }

    return retStatus;
}

STATUS iceAgentConnectedStateSetup(PIceAgent pIceAgent)
{
    STATUS retStatus = STATUS_SUCCESS;
    PDoubleListNode pCurNode = NULL;
    PIceCandidatePair pIceCandidatePair = NULL;

    CHK(pIceAgent != NULL, STATUS_NULL_ARG);

    // Assume holding pIceAgent->lock

    // use the first connected pair as the data sending pair
    CHK_STATUS(doubleListGetHeadNode(pIceAgent->iceCandidatePairs, &pCurNode));
    while (pCurNode != NULL) {
        pIceCandidatePair = (PIceCandidatePair) pCurNode->data;
        pCurNode = pCurNode->pNext;

        if (pIceCandidatePair->state == ICE_CANDIDATE_PAIR_STATE_SUCCEEDED) {
            pIceAgent->pDataSendingIceCandidatePair = pIceCandidatePair;
            break;
        }
    }

    // schedule sending keep alive
    CHK_STATUS(timerQueueAddTimer(pIceAgent->timerQueueHandle,
                                  KVS_ICE_DEFAULT_TIMER_START_DELAY,
                                  KVS_ICE_SEND_KEEP_ALIVE_INTERVAL,
                                  iceAgentSendKeepAliveTimerCallback,
                                  (UINT64) pIceAgent,
                                  &pIceAgent->keepAliveTimerTask));

CleanUp:

    CHK_LOG_ERR(retStatus);

    if (STATUS_FAILED(retStatus)) {
        iceAgentFatalError(pIceAgent, retStatus);
    }

    return retStatus;
}

STATUS iceAgentNominatingStateSetup(PIceAgent pIceAgent)
{
    STATUS retStatus = STATUS_SUCCESS;

    CHK(pIceAgent != NULL, STATUS_NULL_ARG);

    // Assume holding pIceAgent->lock

    if (pIceAgent->isControlling) {
        CHK_STATUS(iceAgentNominateCandidatePair(pIceAgent));

        if (pIceAgent->pBindingRequest != NULL) {
            CHK_STATUS(freeStunPacket(&pIceAgent->pBindingRequest));
        }
        CHK_STATUS(createStunPacket(STUN_PACKET_TYPE_BINDING_REQUEST, NULL, &pIceAgent->pBindingRequest));
        CHK_STATUS(appendStunUsernameAttribute(pIceAgent->pBindingRequest, pIceAgent->combinedUserName));
        CHK_STATUS(appendStunPriorityAttribute(pIceAgent->pBindingRequest, 0));
        CHK_STATUS(appendStunIceControllAttribute(
                pIceAgent->pBindingRequest,
                STUN_ATTRIBUTE_TYPE_ICE_CONTROLLING,
                pIceAgent->tieBreaker));
        CHK_STATUS(appendStunFlagAttribute(pIceAgent->pBindingRequest, STUN_ATTRIBUTE_TYPE_USE_CANDIDATE));
    }

    pIceAgent->stateEndTime = GETTIME() + pIceAgent->kvsRtcConfiguration.iceCandidateNominationTimeout;

CleanUp:

    CHK_LOG_ERR(retStatus);

    if (STATUS_FAILED(retStatus)) {
        iceAgentFatalError(pIceAgent, retStatus);
    }

    return retStatus;
}

STATUS iceAgentReadyStateSetup(PIceAgent pIceAgent)
{
    STATUS retStatus = STATUS_SUCCESS;
    PIceCandidatePair pNominatedAndValidCandidatePair = NULL;
    CHAR ipAddrStr[KVS_IP_ADDRESS_STRING_BUFFER_LEN];
    PDoubleListNode pCurNode = NULL;
    PIceCandidatePair pIceCandidatePair = NULL;
    UINT32 i;
    PTurnConnectionTracker pSelectedTurnConnectionTracker = NULL;

    CHK(pIceAgent != NULL, STATUS_NULL_ARG);

    CHK_STATUS(timerQueueUpdateTimerPeriod(pIceAgent->timerQueueHandle, (UINT64) pIceAgent,
                                           pIceAgent->iceAgentStateTimerTask, KVS_ICE_STATE_READY_TIMER_POLLING_INTERVAL));

    // find nominated pair
    CHK_STATUS(doubleListGetHeadNode(pIceAgent->iceCandidatePairs, &pCurNode));
    while (pCurNode != NULL && pNominatedAndValidCandidatePair == NULL) {
        pIceCandidatePair = (PIceCandidatePair) pCurNode->data;
        pCurNode = pCurNode->pNext;

        if (pIceCandidatePair->nominated && pIceCandidatePair->state == ICE_CANDIDATE_PAIR_STATE_SUCCEEDED) {
            pNominatedAndValidCandidatePair = pIceCandidatePair;
            break;
        }
    }

    CHK(pNominatedAndValidCandidatePair != NULL, STATUS_ICE_NO_NOMINATED_VALID_CANDIDATE_PAIR_AVAILABLE);

    pIceAgent->pDataSendingIceCandidatePair = pNominatedAndValidCandidatePair;
    CHK_STATUS(getIpAddrStr(&pIceAgent->pDataSendingIceCandidatePair->local->ipAddress,
                            ipAddrStr,
                            ARRAY_SIZE(ipAddrStr)));
    DLOGD("Selected pair ip address: %s, port %u, local candidate type: %s",
          ipAddrStr,
          (UINT16) getInt16(pIceAgent->pDataSendingIceCandidatePair->local->ipAddress.port),
          iceAgentGetCandidateTypeStr(pIceAgent->pDataSendingIceCandidatePair->local->iceCandidateType));

    /* no state timeout for ready state */
    pIceAgent->stateEndTime = INVALID_TIMESTAMP_VALUE;

    /* free turn allocations that are not needed. At most one turn allocation will remain after this. */
    if (pIceAgent->turnConnectionTrackerCount > 0) {
        DLOGD("Freeing Turn allocations that are not selected. Total turn allocation count %u", pIceAgent->turnConnectionTrackerCount);
        for (i = 0; i < pIceAgent->turnConnectionTrackerCount; ++i) {
            /* dont free selected candidate's turn allocation */
            if (pIceAgent->turnConnectionTrackers[i].pRelayCandidate != pIceAgent->pDataSendingIceCandidatePair->local) {
                CHK_STATUS(turnConnectionShutdown(pIceAgent->turnConnectionTrackers[i].pTurnConnection, 0));
                pIceAgent->turnConnectionTrackers[i].freeTurnConnectionEndTime = GETTIME() + KVS_ICE_TURN_CONNECTION_SHUTDOWN_TIMEOUT;
                CHK_STATUS(timerQueueAddTimer(pIceAgent->timerQueueHandle, KVS_ICE_DEFAULT_TIMER_START_DELAY, HUNDREDS_OF_NANOS_IN_A_SECOND,
                                              iceAgentFreeTurnConnectionTimerCallback, (UINT64) &pIceAgent->turnConnectionTrackers[i],
                                              &pIceAgent->turnConnectionTrackers[i].freeTurnConnectionTimerId));
                pIceAgent->turnConnectionTrackers[i].pRelayCandidate->state = ICE_CANDIDATE_STATE_INVALID;
            } else {
                pSelectedTurnConnectionTracker = &pIceAgent->turnConnectionTrackers[i];
            }
        }
        CHK_STATUS(iceAgentInvalidateCandidatePair(pIceAgent));

        if (pSelectedTurnConnectionTracker != NULL) {
            pIceAgent->turnConnectionTrackers[i] = *pSelectedTurnConnectionTracker;
            pIceAgent->turnConnectionTrackerCount = 1;
        } else {
            pIceAgent->turnConnectionTrackerCount = 0;
        }
    }

CleanUp:

    CHK_LOG_ERR(retStatus);

    if (STATUS_FAILED(retStatus)) {
        iceAgentFatalError(pIceAgent, retStatus);
    }

    return retStatus;
}

STATUS iceAgentNominateCandidatePair(PIceAgent pIceAgent)
{
    ENTERS();

    STATUS retStatus = STATUS_SUCCESS;
    PIceCandidatePair pNominatedCandidatePair = NULL, pIceCandidatePair = NULL;
    UINT32 iceCandidatePairsCount = FALSE;
    PDoubleListNode pCurNode = NULL;

    CHK(pIceAgent != NULL, STATUS_NULL_ARG);

    // Assume holding pIceAgent->lock
    // do nothing if not controlling
    CHK(pIceAgent->isControlling, retStatus);

    DLOGD("Nominating candidate pair");

    CHK_STATUS(doubleListGetNodeCount(pIceAgent->iceCandidatePairs, &iceCandidatePairsCount));
    CHK(iceCandidatePairsCount > 0 , STATUS_ICE_CANDIDATE_PAIR_LIST_EMPTY);

    CHK_STATUS(doubleListGetHeadNode(pIceAgent->iceCandidatePairs, &pCurNode));
    while (pCurNode != NULL && pNominatedCandidatePair == NULL) {
        pIceCandidatePair = (PIceCandidatePair) pCurNode->data;
        pCurNode = pCurNode->pNext;

        // nominate first connected iceCandidatePair. it should have the highest priority since
        // iceCandidatePairs is already sorted by priority.
        if (pIceCandidatePair->state == ICE_CANDIDATE_PAIR_STATE_SUCCEEDED) {
            pNominatedCandidatePair = pIceCandidatePair;
        }
    }

    // should have a nominated pair.
    CHK(pNominatedCandidatePair != NULL, STATUS_ICE_FAILED_TO_NOMINATE_CANDIDATE_PAIR);

    pNominatedCandidatePair->nominated = TRUE;

    // reset transaction id list to ignore future connectivity check response.
    transactionIdStoreClear(pNominatedCandidatePair->pTransactionIdStore);

    // move not nominated candidate pairs to frozen state so the second connectivity check only checks the nominated pair.
    CHK_STATUS(doubleListGetHeadNode(pIceAgent->iceCandidatePairs, &pCurNode));
    while (pCurNode != NULL) {
        pIceCandidatePair = (PIceCandidatePair) pCurNode->data;
        pCurNode = pCurNode->pNext;

        if (!pIceCandidatePair->nominated) {
            pIceCandidatePair->state = ICE_CANDIDATE_PAIR_STATE_FROZEN;
        }
    }

CleanUp:

    CHK_LOG_ERR(retStatus);

    LEAVES();
    return retStatus;
}

STATUS iceAgentInvalidateCandidatePair(PIceAgent pIceAgent)
{
    ENTERS();

    STATUS retStatus = STATUS_SUCCESS;
    PIceCandidatePair pIceCandidatePair = NULL;
    PDoubleListNode pCurNode = NULL;

    // Assume holding pIceAgent->lock
    CHK(pIceAgent != NULL, STATUS_NULL_ARG);

    CHK_STATUS(doubleListGetHeadNode(pIceAgent->iceCandidatePairs, &pCurNode));
    while (pCurNode != NULL) {
        pIceCandidatePair = (PIceCandidatePair) pCurNode->data;
        pCurNode = pCurNode->pNext;

        if (pIceCandidatePair->local->state != ICE_CANDIDATE_STATE_VALID) {
            pIceCandidatePair->state = ICE_CANDIDATE_PAIR_STATE_FAILED;
        }
    }

CleanUp:

    CHK_LOG_ERR(retStatus);

    LEAVES();
    return retStatus;
}

STATUS incomingRelayedDataHandler(UINT64 customData, PSocketConnection pSocketConnection, PBYTE pBuffer, UINT32 bufferLen,
                                  PKvsIpAddress pSrc, PKvsIpAddress pDest)
{
    STATUS retStatus = STATUS_SUCCESS;
    PIceCandidate pRelayedCandidate = (PIceCandidate) customData;
    // this should be more than enough. Usually the number of channel data in each tcp message is around 4
    TurnChannelData turnChannelData[DEFAULT_TURN_CHANNEL_DATA_BUFFER_SIZE];
    UINT32 turnChannelDataCount = ARRAY_SIZE(turnChannelData), i = 0;

    CHK(pRelayedCandidate != NULL && pSocketConnection != NULL, STATUS_NULL_ARG);

    CHK_STATUS(turnConnectionIncomingDataHandler(pRelayedCandidate->pTurnConnectionTracker->pTurnConnection, pBuffer, bufferLen, pSrc, pDest,
                                                 turnChannelData, &turnChannelDataCount));
    for (; i < turnChannelDataCount; ++i) {
        incomingDataHandler((UINT64) pRelayedCandidate->pTurnConnectionTracker->pIceAgent,
                            pSocketConnection,
                            turnChannelData[i].data,
                            turnChannelData[i].size,
                            &turnChannelData[i].senderAddr,
                            NULL);
    }

CleanUp:
    CHK_LOG_ERR(retStatus);

    return retStatus;
}

STATUS incomingDataHandler(UINT64 customData, PSocketConnection pSocketConnection, PBYTE pBuffer, UINT32 bufferLen,
                           PKvsIpAddress pSrc, PKvsIpAddress pDest)
{
    STATUS retStatus = STATUS_SUCCESS;
    PIceAgent pIceAgent = (PIceAgent) customData;
    BOOL locked = FALSE;

    CHK(pIceAgent != NULL && pSocketConnection != NULL, STATUS_NULL_ARG);

    MUTEX_LOCK(pIceAgent->lock);
    locked = TRUE;

    pIceAgent->lastDataReceivedTime = GETTIME();

    // for stun packets, first 8 bytes are 4 byte type and length, then 4 byte magic byte
    if ((bufferLen < 8 || !IS_STUN_PACKET(pBuffer)) && pIceAgent->iceAgentCallbacks.inboundPacketFn != NULL) {
        // release lock early
        MUTEX_UNLOCK(pIceAgent->lock);
        locked = FALSE;
        pIceAgent->iceAgentCallbacks.inboundPacketFn(pIceAgent->iceAgentCallbacks.customData, pBuffer, bufferLen);
    } else {
        CHK_STATUS(handleStunPacket(pIceAgent, pBuffer, bufferLen, pSocketConnection, pSrc, pDest));
    }

CleanUp:
    CHK_LOG_ERR(retStatus);

    if (locked) {
        MUTEX_UNLOCK(pIceAgent->lock);
    }

    return retStatus;
}

STATUS iceCandidateSerialize(PIceCandidate pIceCandidate, PCHAR pOutputData, PUINT32 pOutputLength)
{
    STATUS retStatus = STATUS_SUCCESS;
    INT32 amountWritten = 0;

    CHK(pIceCandidate != NULL && pOutputLength != NULL, STATUS_NULL_ARG);

    // TODO FIXME real source of randomness
    if (IS_IPV4_ADDR(&(pIceCandidate->ipAddress))) {
        amountWritten = SNPRINTF(pOutputData,
                                 pOutputData == NULL ? 0 : *pOutputLength,
                                 "%u 1 udp %u %d.%d.%d.%d %d typ %s raddr 0.0.0.0 rport 0 generation 0 network-cost 999",
                                 pIceCandidate->foundation,
                                 pIceCandidate->priority,
                                 pIceCandidate->ipAddress.address[0],
                                 pIceCandidate->ipAddress.address[1],
                                 pIceCandidate->ipAddress.address[2],
                                 pIceCandidate->ipAddress.address[3],
                                 (UINT16) getInt16(pIceCandidate->ipAddress.port),
                                 iceAgentGetCandidateTypeStr(pIceCandidate->iceCandidateType));

        CHK_WARN(amountWritten > 0, STATUS_INTERNAL_ERROR, "SNPRINTF failed");

        if (pOutputData == NULL) {
            *pOutputLength = ((UINT32) amountWritten) + 1; // +1 for null terminator
        } else {
            // amountWritten doesnt account for null char
            CHK(amountWritten < (INT32) *pOutputLength, STATUS_BUFFER_TOO_SMALL);
        }

    } else {
        DLOGW("ipv6 not supported yet");
    }

CleanUp:

    return retStatus;
}

STATUS handleStunPacket(PIceAgent pIceAgent, PBYTE pBuffer, UINT32 bufferLen, PSocketConnection pSocketConnection,
                        PKvsIpAddress pSrcAddr, PKvsIpAddress pDestAddr)
{
    UNUSED_PARAM(pDestAddr);

    STATUS retStatus = STATUS_SUCCESS;
    PStunPacket pStunPacket = NULL, pStunResponse = NULL;
    PStunAttributeHeader pStunAttr = NULL;
    UINT16 stunPacketType = 0;
    PIceCandidatePair pIceCandidatePair = NULL;
    PStunAttributeAddress pStunAttributeAddress = NULL;
    PStunAttributePriority pStunAttributePriority = NULL;
    UINT32 priority = 0;
    PIceCandidate pIceCandidate = NULL;
    CHAR ipAddrStr[KVS_IP_ADDRESS_STRING_BUFFER_LEN], ipAddrStr2[KVS_IP_ADDRESS_STRING_BUFFER_LEN];
    PCHAR hexStr = NULL;
    UINT32 hexStrLen = 0;

    // need to determine stunPacketType before deserializing because different password should be used depending on the packet type
    stunPacketType = (UINT16) getInt16(*((PUINT16) pBuffer));

    switch (stunPacketType) {
        case STUN_PACKET_TYPE_BINDING_REQUEST:
            CHK_STATUS(deserializeStunPacket(pBuffer, bufferLen, (PBYTE) pIceAgent->localPassword, (UINT32) STRLEN(pIceAgent->localPassword) * SIZEOF(CHAR), &pStunPacket));
            CHK_STATUS(createStunPacket(STUN_PACKET_TYPE_BINDING_RESPONSE_SUCCESS, pStunPacket->header.transactionId, &pStunResponse));
            CHK_STATUS(appendStunAddressAttribute(pStunResponse, STUN_ATTRIBUTE_TYPE_XOR_MAPPED_ADDRESS, pSrcAddr));
            CHK_STATUS(appendStunIceControllAttribute(
                    pStunResponse,
                    pIceAgent->isControlling ? STUN_ATTRIBUTE_TYPE_ICE_CONTROLLING : STUN_ATTRIBUTE_TYPE_ICE_CONTROLLED,
                    pIceAgent->tieBreaker
            ));

            CHK_STATUS(getStunAttribute(pStunPacket, STUN_ATTRIBUTE_TYPE_PRIORITY, (PStunAttributeHeader *) &pStunAttributePriority));
            priority = pStunAttributePriority == NULL ? 0 : pStunAttributePriority->priority;
            CHK_STATUS(iceAgentCheckPeerReflexiveCandidate(pIceAgent, pSrcAddr, priority, TRUE, 0));

            CHK_STATUS(findCandidateWithSocketConnection(pSocketConnection, pIceAgent->localCandidates, &pIceCandidate));
            CHK_WARN(pIceCandidate != NULL, retStatus, "Could not find local candidate to send STUN response");
            CHK_STATUS(iceAgentSendStunPacket(pStunResponse,
                                              (PBYTE) pIceAgent->localPassword,
                                              (UINT32) STRLEN(pIceAgent->localPassword) * SIZEOF(CHAR),
                                              pIceAgent,
                                              pIceCandidate,
                                              pSrcAddr));

            // return early if there is no candidate pair. This can happen when we get connectivity check from the peer
            // before we receive the answer.
            CHK_STATUS(findIceCandidatePairWithLocalSocketConnectionAndRemoteAddr(pIceAgent, pSocketConnection,
                                                                                  pSrcAddr, TRUE, &pIceCandidatePair));
            CHK(pIceCandidatePair != NULL, retStatus);

            if (!pIceCandidatePair->nominated) {
                CHK_STATUS(getStunAttribute(pStunPacket, STUN_ATTRIBUTE_TYPE_USE_CANDIDATE, &pStunAttr));
                if (pStunAttr != NULL) {
                    DLOGD("received candidate with USE_CANDIDATE flag, local candidate type %s.",
                          iceAgentGetCandidateTypeStr(pIceCandidatePair->local->iceCandidateType));
                    pIceCandidatePair->nominated = TRUE;
                }
            }

            // schedule a connectivity check for the pair
            if (pIceCandidatePair->state == ICE_CANDIDATE_PAIR_STATE_FROZEN ||
                pIceCandidatePair->state == ICE_CANDIDATE_PAIR_STATE_WAITING ||
                pIceCandidatePair->state == ICE_CANDIDATE_PAIR_STATE_IN_PROGRESS) {
                CHK_STATUS(stackQueueEnqueue(pIceAgent->triggeredCheckQueue, (UINT64) pIceCandidatePair));
            }

            break;

        case STUN_PACKET_TYPE_BINDING_RESPONSE_SUCCESS:

            // check if Binding Response is for finding srflx candidate
            if (transactionIdStoreHasId(pIceAgent->pStunBindingRequestTransactionIdStore, pBuffer + STUN_PACKET_TRANSACTION_ID_OFFSET)) {
                CHK_STATUS(findCandidateWithSocketConnection(pSocketConnection, pIceAgent->localCandidates, &pIceCandidate));
                CHK_WARN(pIceCandidate != NULL, retStatus,
                         "Local candidate with socket %d not found. Dropping STUN binding success response",
                         pSocketConnection->localSocket);

                CHK_STATUS(deserializeStunPacket(pBuffer, bufferLen, NULL, 0, &pStunPacket));
                CHK_STATUS(getStunAttribute(pStunPacket, STUN_ATTRIBUTE_TYPE_XOR_MAPPED_ADDRESS, &pStunAttr));
                CHK_WARN(pStunAttr != NULL, retStatus, "No mapped address attribute found in STUN binding response. Dropping Packet");

                pStunAttributeAddress = (PStunAttributeAddress) pStunAttr;
                CHK_STATUS(updateCandidateAddress(pIceCandidate, &pStunAttributeAddress->address));
                CHK_STATUS(iceAgentReportNewLocalCandidate(pIceAgent, pIceCandidate));
                CHK(FALSE, retStatus);
            }

            CHK_STATUS(findIceCandidatePairWithLocalSocketConnectionAndRemoteAddr(pIceAgent, pSocketConnection, pSrcAddr, TRUE, &pIceCandidatePair));
            if (pIceCandidatePair == NULL) {
                CHK_STATUS(getIpAddrStr(pSrcAddr, ipAddrStr, ARRAY_SIZE(ipAddrStr)));
                CHK_STATUS(getIpAddrStr(&pSocketConnection->hostIpAddr, ipAddrStr2, ARRAY_SIZE(ipAddrStr2)));
                CHK_WARN(FALSE, retStatus, "Cannot find candidate pair with local candidate %s and remote candidate %s. Dropping STUN binding success response",
                         ipAddrStr2, ipAddrStr);
            }

            CHK_WARN(transactionIdStoreHasId(pIceCandidatePair->pTransactionIdStore, pBuffer + STUN_PACKET_TRANSACTION_ID_OFFSET), retStatus,
                     "Dropping response packet because transaction id does not match");
            CHK_STATUS(deserializeStunPacket(pBuffer, bufferLen, (PBYTE) pIceAgent->remotePassword, (UINT32) STRLEN(pIceAgent->remotePassword) * SIZEOF(CHAR), &pStunPacket));
            CHK_STATUS(getStunAttribute(pStunPacket, STUN_ATTRIBUTE_TYPE_XOR_MAPPED_ADDRESS, &pStunAttr));
            CHK_WARN(pStunAttr != NULL, retStatus, "No mapped address attribute found in STUN response. Dropping Packet");

            pStunAttributeAddress = (PStunAttributeAddress) pStunAttr;

            if (!isSameIpAddress(&pStunAttributeAddress->address, &pIceCandidatePair->local->ipAddress, FALSE)) {
                // this can happen for host and server reflexive candidates. If the peer
                // is in the same subnet, server reflexive candidate's binding response's xor mapped ip address will be
                // the host candidate ip address. In this case we will ignore the packet since the host candidate will
                // be getting its own response for the connection check.
                DLOGD("local candidate ip address does not match with xor mapped address in binding response");

                // we have a peer reflexive local candidate
                CHK_STATUS(iceAgentCheckPeerReflexiveCandidate(pIceAgent, &pStunAttributeAddress->address, pIceCandidatePair->local->priority, FALSE, pSocketConnection));

                CHK(FALSE, retStatus);
            }

            pIceCandidatePair->state = ICE_CANDIDATE_PAIR_STATE_SUCCEEDED;

            break;
        case STUN_PACKET_TYPE_BINDING_INDICATION:
            DLOGD("Received STUN binding indication");
            break;

        default:
            CHK_STATUS(hexEncode(pBuffer, bufferLen, NULL, &hexStrLen));
            hexStr = MEMCALLOC(1, hexStrLen * SIZEOF(CHAR));
            CHK(hexStr != NULL, STATUS_NOT_ENOUGH_MEMORY);
            CHK_STATUS(hexEncode(pBuffer, bufferLen, hexStr, &hexStrLen));
            DLOGW("Dropping unrecognized STUN packet. Packet type: 0x%02x. Packet content: \n\t%s", stunPacketType, hexStr);
            MEMFREE(hexStr);
            break;
    }

CleanUp:

    CHK_LOG_ERR(retStatus);

    if (pStunPacket != NULL) {
        freeStunPacket(&pStunPacket);
    }

    if (pStunResponse != NULL) {
        freeStunPacket(&pStunResponse);
    }

    // TODO send error packet

    return retStatus;
}

STATUS iceAgentCheckPeerReflexiveCandidate(PIceAgent pIceAgent, PKvsIpAddress pIpAddress, UINT32 priority, BOOL isRemote, PSocketConnection pSocketConnection)
{
    STATUS retStatus = STATUS_SUCCESS;
    PIceCandidate pIceCandidate = NULL, pLocalIceCandidate = NULL;
    BOOL freeIceCandidateOnError = TRUE;
    UINT32 candidateCount;

    // remote candidate dont have socketConnection
    CHK(pIceAgent != NULL && pIpAddress != NULL && (isRemote || pSocketConnection != NULL), STATUS_NULL_ARG);

    if (!isRemote) {
        // local peer reflexive candidate replaces existing local candidate because the peer sees different address
        // for this local candidate.
        CHK_STATUS(findCandidateWithIp(pIpAddress, pIceAgent->localCandidates, &pIceCandidate));
        CHK(pIceCandidate == NULL, retStatus); // return early if duplicated

        findCandidateWithSocketConnection(pSocketConnection, pIceAgent->localCandidates, &pLocalIceCandidate);
        pLocalIceCandidate->iceCandidateType = ICE_CANDIDATE_TYPE_PEER_REFLEXIVE;
        pLocalIceCandidate->ipAddress = *pIpAddress;
        DLOGD("New local peer reflexive candidate found");
        CHK(FALSE, retStatus);
    }

    CHK_STATUS(doubleListGetNodeCount(pIceAgent->remoteCandidates, &candidateCount));
    CHK_WARN(candidateCount < KVS_ICE_MAX_REMOTE_CANDIDATE_COUNT, retStatus, "max remote candidate count exceeded"); // return early if limit exceeded
    CHK_STATUS(findCandidateWithIp(pIpAddress, pIceAgent->remoteCandidates, &pIceCandidate));
    CHK(pIceCandidate == NULL, retStatus); // return early if duplicated
    DLOGD("New remote peer reflexive candidate found");

    CHK((pIceCandidate = MEMCALLOC(1, SIZEOF(IceCandidate))) != NULL, STATUS_NOT_ENOUGH_MEMORY);
    pIceCandidate->ipAddress = *pIpAddress;
    pIceCandidate->iceCandidateType = ICE_CANDIDATE_TYPE_PEER_REFLEXIVE;
    pIceCandidate->priority = priority;
    pIceCandidate->state = ICE_CANDIDATE_STATE_VALID;
    pIceCandidate->pSocketConnection = NULL; // remote candidate dont have PSocketConnection

    CHK_STATUS(doubleListInsertItemHead(pIceAgent->remoteCandidates, (UINT64) pIceCandidate));
    freeIceCandidateOnError = FALSE;

    CHK_STATUS(createIceCandidatePairs(pIceAgent, pIceCandidate, isRemote));

CleanUp:

    CHK_LOG_ERR(retStatus);

    if (STATUS_FAILED(retStatus) && freeIceCandidateOnError) {
        MEMFREE(pIceCandidate);
    }

    return retStatus;
}

STATUS iceAgentFatalError(PIceAgent pIceAgent, STATUS errorStatus)
{
    STATUS retStatus = STATUS_SUCCESS;
    CHK(pIceAgent != NULL, STATUS_NULL_ARG);

    MUTEX_LOCK(pIceAgent->lock);
    pIceAgent->iceAgentStatus = errorStatus;
    MUTEX_UNLOCK(pIceAgent->lock);

CleanUp:

    return retStatus;
}

STATUS updateCandidateAddress(PIceCandidate pIceCandidate, PKvsIpAddress pIpAddr)
{
    STATUS retStatus = STATUS_SUCCESS;

    CHK(pIceCandidate != NULL && pIpAddr != NULL, STATUS_NULL_ARG);
    CHK(pIceCandidate->iceCandidateType != ICE_CANDIDATE_TYPE_HOST, STATUS_INVALID_ARG);
    CHK(pIceCandidate->state == ICE_CANDIDATE_STATE_NEW, retStatus);

    MEMCPY(pIceCandidate->ipAddress.address, pIpAddr->address, IS_IPV4_ADDR(pIpAddr) ? IPV4_ADDRESS_LENGTH : IPV6_ADDRESS_LENGTH);
    pIceCandidate->ipAddress.port = pIpAddr->port;

    pIceCandidate->state = ICE_CANDIDATE_STATE_VALID;

    if (pIceCandidate->iceCandidateType == ICE_CANDIDATE_TYPE_SERVER_REFLEXIVE) {
        DLOGD("New server reflexive candidate found");
    } else {
        DLOGD("New relay candidate found");
    }

CleanUp:

    return retStatus;
}

UINT32 computeCandidatePriority(PIceCandidate pIceCandidate)
{
    UINT32 typePreference = 0, localPreference = 0;

    switch (pIceCandidate->iceCandidateType) {
        case ICE_CANDIDATE_TYPE_HOST:
            typePreference = ICE_PRIORITY_HOST_CANDIDATE_TYPE_PREFERENCE;
            break;
        case ICE_CANDIDATE_TYPE_SERVER_REFLEXIVE:
            typePreference = ICE_PRIORITY_SERVER_REFLEXIVE_CANDIDATE_TYPE_PREFERENCE;
            break;
        case ICE_CANDIDATE_TYPE_PEER_REFLEXIVE:
            typePreference = ICE_PRIORITY_PEER_REFLEXIVE_CANDIDATE_TYPE_PREFERENCE;
            break;
        case ICE_CANDIDATE_TYPE_RELAYED:
            typePreference = ICE_PRIORITY_RELAYED_CANDIDATE_TYPE_PREFERENCE;
            break;
    }

    if (!pIceCandidate->ipAddress.isPointToPoint) {
        localPreference = ICE_PRIORITY_LOCAL_PREFERENCE;
    }

    return (2 ^ 24) * (typePreference) + (2 ^ 8) * (localPreference) + 255;
}

UINT64 computeCandidatePairPriority(PIceCandidatePair pIceCandidatePair, BOOL isLocalControlling)
{
    UINT64 controllingAgentCandidatePri = pIceCandidatePair->local->priority;
    UINT64 controlledAgentCandidatePri = pIceCandidatePair->remote->priority;

    if (!isLocalControlling) {
        controllingAgentCandidatePri = controlledAgentCandidatePri;
        controlledAgentCandidatePri = pIceCandidatePair->local->priority;
    }

    // https://tools.ietf.org/html/rfc5245#appendix-B.5
    return ((UINT64) 1 << 32) * MIN(controlledAgentCandidatePri, controllingAgentCandidatePri) +
           2 * MAX(controlledAgentCandidatePri, controllingAgentCandidatePri) +
           (controllingAgentCandidatePri > controlledAgentCandidatePri ? 1 : 0);
}

PCHAR iceAgentGetCandidateTypeStr(ICE_CANDIDATE_TYPE candidateType) {
    switch(candidateType) {
        case ICE_CANDIDATE_TYPE_HOST:
            return SDP_CANDIDATE_TYPE_HOST;
        case ICE_CANDIDATE_TYPE_SERVER_REFLEXIVE:
            return SDP_CANDIDATE_TYPE_SERFLX;
        case ICE_CANDIDATE_TYPE_PEER_REFLEXIVE:
            return SDP_CANDIDATE_TYPE_PRFLX;
        case ICE_CANDIDATE_TYPE_RELAYED:
            return SDP_CANDIDATE_TYPE_RELAY;
    }
}


