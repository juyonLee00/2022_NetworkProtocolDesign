#include "mbed.h"
#include "string.h"

#include "ARQ_FSMevent.h"
#include "ARQ_msg.h"
#include "ARQ_timer.h"
#include "ARQ_LLinterface.h"
#include "ARQ_parameters.h"

//FSM state -------------------------------------------------
#define MAINSTATE_IDLE              0
#define MAINSTATE_TX                1
#define MAINSTATE_ACK               2

//GLOBAL variables (DO NOT TOUCH!) ------------------------------------------
//serial port interface
Serial pc(USBTX, USBRX);

//state variables
uint8_t main_state = MAINSTATE_IDLE; //protocol state

//source/destination ID
uint8_t endNode_ID=1;
uint8_t dest_ID=0;

//PDU context/size
uint8_t arqPdu[200];
uint8_t pduSize;

//SDU (input)
uint8_t originalWord[200];
uint8_t wordLen=0;



//ARQ parameters -------------------------------------------------------------
uint8_t seqNum = 0;     //ARQ sequence number
uint8_t retxCnt = 0;    //ARQ retransmission counter
uint8_t arqAck[5];      //ARQ ACK PDU


//application event handler : generating SDU from keyboard input
void arqMain_processInputWord(void)
{
    char c = pc.getc();
    if (main_state == MAINSTATE_IDLE &&
        !arqEvent_checkEventFlag(arqEvent_dataToSend))
    {
        if (c == '\n' || c == '\r')
        {
            originalWord[wordLen++] = '\0';
            arqEvent_setEventFlag(arqEvent_dataToSend);
            pc.printf("word is ready! ::: %s\n", originalWord);
        }
        else
        {
            originalWord[wordLen++] = c;
            if (wordLen >= ARQMSG_MAXDATASIZE-1)
            {
                originalWord[wordLen++] = '\0';
                arqEvent_setEventFlag(arqEvent_dataToSend);
                pc.printf("\n max reached! word forced to be ready :::: %s\n", originalWord);
            }
        }
    }
}




//FSM operation implementation ------------------------------------------------
int main(void){
    uint8_t flag_needPrint=1;
    uint8_t prev_state = 0;

    //initialization
    pc.printf("------------------ ARQ protocol starts! --------------------------\n");
    arqEvent_clearAllEventFlag();
    
    //source & destination ID setting
    pc.printf(":: ID for this node : ");
    pc.scanf("%d", &endNode_ID);
    pc.printf(":: ID for the destination : ");
    pc.scanf("%d", &dest_ID);
    pc.getc();

    pc.printf("endnode : %i, dest : %i\n", endNode_ID, dest_ID);

    arqLLI_initLowLayer(endNode_ID);
    pc.attach(&arqMain_processInputWord, Serial::RxIrq);





    while(1)
    {
        //debug message
        if (prev_state != main_state)
        {
            debug_if(DBGMSG_ARQ, "[ARQ] State transition from %i to %i\n", prev_state, main_state);
            prev_state = main_state;
        }


        //FSM should be implemented here! ---->>>>
        switch (main_state)
        {
            case MAINSTATE_IDLE: //IDLE state description
                //e. DATA_IND(data)?
                if (arqEvent_checkEventFlag(arqEvent_dataRcvd)) //if data reception event happens
                {
                    //Retrieving data info. 정보 검색
                    uint8_t srcId = arqLLI_getSrcId();
                    uint8_t* dataPtr = arqLLI_getRcvdDataPtr();
                    uint8_t size = arqLLI_getSize();
                    uint8_t rxSeqNum = arqMsg_getSeq(dataPtr);

                    pc.printf("\n -------------------------------------------------\nRCVD from %i : %s (length:%i, seq:%i)\n -------------------------------------------------\n", 
                                srcId, arqMsg_getWord(dataPtr), size, rxSeqNum);

                    //ACK 만들기 (인코딩)
                    arqMsg_encodeAck(arqAck, rxSeqNum);
                    //인코딩한 ACK 보내기
                    arqLLI_sendData(arqAck, ARQMSG_ACKSIZE, srcId);

                    main_state = MAINSTATE_TX;
                    flag_needPrint = 1;

                    arqEvent_clearEventFlag(arqEvent_dataRcvd);
                }
                //a. SDU in
                else if (arqEvent_checkEventFlag(arqEvent_dataToSend)) //if data needs to be sent (keyboard input)
                {
                    //msg header setting - PDU 인코딩하여 보내기
                    pduSize = arqMsg_encodeData(arqPdu, originalWord, seqNum, wordLen);
                    arqLLI_sendData(arqPdu, pduSize, dest_ID);

                    seqNum = (seqNum+1)%ARQMSSG_MAX_SEQNUM; ////최대값 넘지않게 나머지 연산자로 계산
                    retxCnt = 0;

                    pc.printf("[MAIN] sending to %i (seq:%i)\n", dest_ID, (seqNum-1)%ARQMSSG_MAX_SEQNUM);

                    main_state = MAINSTATE_TX;
                    flag_needPrint = 1;

                    wordLen = 0;
                    arqEvent_clearEventFlag(arqEvent_dataToSend);
                }
                //Error Handling
                else if(arqEvent_checkEventFlag(arqEvent_dataTxDone))
                {
                    pc.printf("[MAIN][ERROR] WARNING!! Cannot happen event %i at state %i\n", arqEvent_dataTxDone, main_state);
                    arqEvent_clearEventFlag(arqEvent_dataTxDone);
                }
                else if(arqEvent_checkEventFlag(arqEvent_ackTxDone))
                {
                    pc.printf("[MAIN][ERROR] WARNING!! Cannot happen event %i at state %i\n", arqEvent_ackTxDone, main_state);
                    arqEvent_clearEventFlag(arqEvent_ackTxDone);
                }
                else if(arqEvent_checkEventFlag(arqEvent_ackRcvd))
                {
                    pc.printf("[MAIN][ERROR] WARNING!! Cannot happen event %i at state %i\n", arqEvent_ackRcvd, main_state);
                    arqEvent_clearEventFlag(arqEvent_ackRcvd);
                }
                else if (flag_needPrint == 1)
                {
                    pc.printf("Give a word to send : ");
                    flag_needPrint = 0;
                }     

                break;

            case MAINSTATE_TX: //TX state description
                //b-1. data에 대한 DATA_CNF
                if (arqEvent_checkEventFlag(arqEvent_dataTxDone)) //data TX finished
                {
                    //타이머 시작
                    arqTimer_startTimer();

                    main_state = MAINSTATE_ACK;
                    arqEvent_clearEventFlag(arqEvent_dataTxDone);
                }
                //b-2. ACK에 대한 DATA_CNF
                else if(arqEvent_checkEventFlag(arqEvent_ackTxDone)){
                    //(c2 조건)타이머가 돌고있는 지 확인하여 스테이트 이동 정함
                    if(arqTimer_getTimerStatus()==0 && arqEvent_checkEventFlag(arqEvent_arqTimeout)== 0){ //타이머가 안돌고 있으면 IDLE로
                        main_state = MAINSTATE_IDLE;
                    }else{
                        main_state = MAINSTATE_ACK; //타이머 돌고있으면 ACK로
                    }
                    arqEvent_clearEventFlag(arqEvent_ackTxDone);
                }
                //Error handling 코드 넣기(a에 대해서만. c와 d는 keep이므로 코드 작성하지 않고 냅둠)
                else if(arqEvent_checkEventFlag(arqEvent_dataToSend)){
                    pc.printf("[MAIN][ERROR] Cannot happen event %i at state %i\n", arqEvent_dataToSend, main_state);
					arqEvent_clearEventFlag(arqEvent_dataToSend);
                }
                break;

            case MAINSTATE_ACK: //ACK state description

                if (arqEvent_checkEventFlag(arqEvent_ackRcvd))
                {
                    pc.printf("ACK received for PDU %i\n",seqNum-1);
                    arqTimer_stopTimer();
                    main_state = MAINSTATE_IDLE;
                    arqEvent_clearEventFlag(arqEvent_ackRcvd);
                }
                else if(arqEvent_checkEventFlag(arqEvent_arqTimeout))
                {
                    if(retxCnt < ARQ_MAXRETRANSMISSION){
                        arqLLI_sendData(arqPdu, pduSize, dest_ID);
						retxCnt = retxCnt + 1;
						main_state = MAINSTATE_TX;
                    }else{
                        main_state = MAINSTATE_IDLE;
                    }
                    arqEvent_clearEventFlag(arqEvent_arqTimeout);
                }
                else if(arqEvent_checkEventFlag(arqEvent_dataRcvd)){
                    uint8_t srcId = arqLLI_getSrcId();
                    uint8_t* dataPtr = arqLLI_getRcvdDataPtr();
                    uint8_t size = arqLLI_getSize();
					uint8_t rxSeqNum = arqMsg_getSeq(dataPtr);

                    pc.printf("\n -------------------------------------------------\nRCVD from %i : %s (length:%i, seq:%i)\n -------------------------------------------------\n", 
                                srcId, arqMsg_getWord(dataPtr), size, rxSeqNum);

					pduSize = arqMsg_encodeAck(arqAck, rxSeqNum);
                    arqLLI_sendData(arqAck, ARQMSG_ACKSIZE, srcId);

                    pc.printf("[MAIN] sending ACK to %i (seq:%i)\n", srcId, rxSeqNum);

                    main_state = MAINSTATE_TX;
                    arqEvent_clearEventFlag(arqEvent_dataRcvd);
                }

                break;

            default :
                break;
        }
    }
}