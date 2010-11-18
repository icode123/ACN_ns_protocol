#include "XYZZY/XYZZY.h"

int hdr_Xyzzy::offset_;


//these two static classes represent the agent and the header
//we aren't sure exactly how they work, just that they are used to
//some how bind the TCL and C++ classes together
static class XyzzyHeaderClass : public PacketHeaderClass {
    public:
        XyzzyHeaderClass() : PacketHeaderClass("PacketHeader/Xyzzy", sizeof(hdr_Xyzzy))\
        {
            bind_offset(&hdr_Xyzzy::offset_);
        }

} class_xyzzyhdr;

static class XyzzyAgentClass : public TclClass {
    public:
        XyzzyAgentClass() : TclClass("Agent/Xyzzy") {}
        TclObject* create(int, const char*const*)
        {
            return (new XyzzyAgent());
        }
} class_xyzzyagent;

//when the retry timer expires it
//calls retryPackets()
void RetryTimer::expire(Event*){
    t_->retryPackets();
}

//this is the constructor, it creates the super and the
//retry timer in the initialization statments
XyzzyAgent::XyzzyAgent() : Agent(PT_XYZZY), seqno_(0), coreTarget(NULL), bufLoc_(0), retry_(this)
{
    //bind the varible to a Tcl varible
    bind("packetSize_", &size_);

    // initialize buffer
    for (int i = 0; i < WINDOW_SIZE; ++i)
        window[i] = NULL;

    //set ackList to null so that the acklist functions
    //don't break when the encounter an empty list
    ackList = NULL;

    //schedule the retry timer
    retry_.sched(RETRY_TIME+0.6);
}

//retry packet resends unacked packets every half second or so
void XyzzyAgent::retryPackets() {

    //we want to get the time and compare it against the time that
    //each packet in our window was sent, if more thatn the RETRY_TIME
    //has elapsed we want to send it again
    double timeNow = Scheduler::instance().clock();
    printf("[%d] Starting retry loop at %f\n", here_.addr_, timeNow);
    for (int i = 0; i < WINDOW_SIZE; ++i) {
        if (window[i] != NULL && timeNow - timeSent[i] > RETRY_TIME) {
            printf("[%d] ** RETRYING PACKET %d **\n", here_.addr_, hdr_Xyzzy::access(window[i])->seqno());

            // send again.
            // this needs to be copied here because recv frees packets when
            // it gets them and if it frees a packet that is still in our window
            // and then it gets called again because it was dropped...Bad things happen
            target_->recv(window[i]->copy());

            // TODO: let buddies know we retried

            //now we increment how many times we have tried to send the packet and update
            //the time stamp
            numTries[i]++;
            timeSent[i] = Scheduler::instance().clock();
        }
    }

    //set the timer to fire again
    if (retry_.status() == TIMER_IDLE)
        retry_.sched(RETRY_TIME);
    else if (retry_.status() == TIMER_PENDING){
        retry_.cancel();
    }
    retry_.resched(RETRY_TIME);
    printf("[%d] Ending retry loop at %f\n", here_.addr_, timeNow);
}

void XyzzyAgent::recordPacket(Packet* pkt, double time) {
    if (window[bufLoc_] != NULL){
        // TODO: delay somehow until there's room for another packet.
        // this is probably better handled in send
        printf("[%d] Send buffer full, but I don't know how to wait yet!, packet lost. (seqno: %d) in the way of (seqno: %d)\n", here_.addr_, hdr_Xyzzy::access(window[bufLoc_])->seqno(), hdr_Xyzzy::access(pkt)->seqno());
    } else {
        // TODO: send to buddies

        //store the packet we just sent in the
        //window and set all the relevant varibles
        window[bufLoc_] = pkt;
        numTries[bufLoc_] = 1;
        timeSent[bufLoc_] = time;

        //move the buffer location forward one.
        //and move it back around if it has moved
        //past the bounds of the array
        bufLoc_++;
        bufLoc_ %= WINDOW_SIZE;
        printf("[%d]  New buffer location: %d\n", here_.addr_, bufLoc_);
    }
}


//this function is called by ns2 when the app attached to this agent wants
//to send a message over the link
void XyzzyAgent::sendmsg(int nbytes, AppData* data, const char* flags) {

    //create a packet to put data in
    Packet* p;

    //if the maximum packet size is zero...somthing has gone terribly wrong
    if (size_ == 0)
        printf("[%d] Xyzzy size_ is 0!\n", here_.addr_);

    //this will eventually handle fragmentation
    if (data && nbytes > size_) {
        printf("[%d] Xyzzy data does not fit in a single packet. Abotr\n", here_.addr_);
        return;
    }

    setupPacket();

    //allocate a new packet from the pool
    p = allocpkt();

    //set the type and size of the packet payload
    //in the common header
    hdr_cmn::access(p)->ptype() = PT_XYZZY;
    hdr_cmn::access(p)->size() = nbytes;

    //set the sequence number and type  of our header
    hdr_Xyzzy::access(p)->seqno() = ++seqno_;
    hdr_Xyzzy::access(p)->type() = T_normal;

    //put our data into the packet
    p->setdata(data);

    //copy the packet because as soon as we pass it to the receive function
    //it will be freed and we need to keep a copy of it just incase it was
    //dropped and needs to be retransmitted
    Packet* pcopy = p->copy();
    //target_->recv(p);
    send(p, 0);
    recordPacket(pcopy, Scheduler::instance().clock());

}

void XyzzyAgent::setupPacket(Packet* pkt){
    DestNode* dest;
    if (pkt){
        // find the dest node matching the source in this packet
        dest = destList;
        while((dest)){
            if (dest->iNsAddr == hdr_ip::access(pkt)->src().addr_
                && dest->iNsPort == hdr_ip::access(pkt)->src().port_){
                break;
            } else {
                dest = dest->next;
            }
        }
        if (dest == NULL)
            dest = primaryDest;
    } else {
        dest = primaryDest;
    }
    // set source info
    if (coreTarget != NULL){
        daddr() = dest->iNsAddr;
        dport() = dest->iNsPort;
        Packet* routingPacket = allocpkt();

        Connector* conn = (Connector*) coreTarget->find(routingPacket);

        IfaceNode* current = ifaceList;

        while(current != NULL){
            if (current->opLink == conn){
                addr() = current->iNsAddr;
                port() = current->iNsPort;
                target_ = current->opTarget;
                break;
            } else {
                current = current->next;
            }
        }
    }

    // set dest info
    daddr() = dest->iNsAddr;
    dport() = dest->iNsPort;
}

//this function is used to add a sequence number to our list of
//received packets when we get a new packet
void XyzzyAgent::updateCumAck(int seqno){
    ackListNode* usefulOne;

    //if the list is empty we want to create a
    //new list node to be the head of the list
    //and set it to be the useful one that we will add thigns to later
    if(!ackList){
        ackList = new ackListNode;
        usefulOne = ackList;

    //if there exists a list, we need to find the point in the list were this
    //packet fits and put it there
    }else{
        ackListNode* current = ackList;

        //crawl through the list of pakcets
        do{
            //if the current node is still less then the seqence number we want to
            //place advance usefulOne and loop again if there is still list
            if(current->seqno < seqno){
                usefulOne = current;

            //if the next element has a larger sequence number
            //we need to escape the loop because useful one contains the node we need
            }else if (current->seqno > seqno){
                break;

            //if we receive a packet we already acked just die
            //noting needs to be done
            }else{
                return;
            }
        }while((current = current->next));

        //once we have found the last node with a lower sequence number
        //we need to create a new node for our sequence numbe and then
        //repair the chain
        usefulOne->next = new ackListNode;
        usefulOne->next->next = current;

        //set useful one to the new node
        usefulOne = usefulOne->next;
    }

    //set the sequence no in useful one because it is now set to the node that
    //needs to be set in either case
    usefulOne->seqno = seqno;

}

//as we receive and ack more messages eventually the end of the list closest to the
//head will become consecutive and thus nolonger nesscary as the head of the list
//represents a cumulative ack, so we prune them off
void XyzzyAgent::ackListPrune(){

    //check to make sure there atleast two
    //elements in the list
    if(!ackList || !ackList->next)
        return;

    //declare book keeping stuff
    ackListNode* usefulOne;
    ackListNode* current = ackList;
    int last_seqno = ackList->seqno;
    int count = 0;

    //step through the list and count the number of consecutive
    //sequence numbers at the beginning. break as soon as you hit a
    //sequence number that is not one more than the previous one
    while((current = current->next)){
        if(current->seqno == last_seqno + 1){
            usefulOne = current;
            last_seqno = current->seqno;
            count++;
        }else {
            break;
        }
    }

    current = ackList;
    ackListNode* n = current;

    //delete the packets we counted before
    for(int i = 0; i < count; ++i){
        if (current->next){
            n = current->next;
            delete current;
            current = n;
        }
    }
    ackList = n;
}

//this is the function called by ns2 when the node this agent is bound
//to receives a message
//
//WARNING:  THIS METHOD RELEASES ANY PACKETS IT RECEIVES...MAKE SURE IF
//A PACKET IS PASSED TO RECEIVE IT IS EITHER COPIED FOR BOOK KEEPING STRUCTURES,
//COPIED FOR RECEIVE, OR YOU ARE HAPPY WITH IT DISSAPPEARING
void XyzzyAgent::recv(Packet* pkt, Handler*) {

    //get the header out of the packet we just got
    hdr_Xyzzy* oldHdr = hdr_Xyzzy::access(pkt);

    //switch on the packet type
    //if it is a normal header
    if (oldHdr->type() == T_normal){

        // Send back an ack, so allocate a packet
        setupPacket(pkt);
        Packet* ap = allocpkt();

        //set up its common header values ie protocol type and size of data
        hdr_cmn::access(ap)->ptype() = PT_XYZZY;
        hdr_cmn::access(ap)->size() = 0;

        //TODO: wrap around oh noes!

        //update the list of acked packets
        //and prune any consecutive ones from the front
        //of the list...we don't need them any more
        updateCumAck(oldHdr->seqno());
        ackListPrune();

        //create a new protocol specific header for the packet
        hdr_Xyzzy* newHdr = hdr_Xyzzy::access(ap);

        //set the fields in our new header to the ones that were
        //in the packet we just received
        newHdr->type() = T_ack;
        newHdr->seqno() = oldHdr->seqno();
        newHdr->cumAck() = ackList->seqno;
        printf("[%d] Generating ack for seqno %d (cumAck: %d)\n", here_.addr_, newHdr->seqno(), newHdr->cumAck());

        // set up ip header
        hdr_ip* newip = hdr_ip::access(ap);
        hdr_ip* oldip = hdr_ip::access(pkt);

        //set the ip header so that the ack packet will
        //go back to the data packet's source
        newip->dst() = oldip->src();
        newip->prio() = oldip->prio();
        newip->flowid() = oldip->flowid();

        //send the ack packet, since it contains a cumulative
        //storage and retransmission is not nesscary
        //target_->recv(ap);
        send(ap, 0);

        // log packet contents to file so we can see what we got
        // and what we lossed and how many copies of each O.o
        char fname[13];
        snprintf(fname, 12, "%d-recv.log", this->addr());
        FILE* lf = fopen(fname, "a");
        if (pkt->userdata()){
            PacketData* data = (PacketData*)pkt->userdata();
            fwrite(data->data(), data->size(), 1, lf);
        } else
            fwrite("NULL Data ", 9, 1, lf);
        fwrite("\n\n\n", 2, 1, lf);
        fclose(lf);

        // Forward data on to application.
        if (app_) {
            app_->process_data(hdr_cmn::access(pkt)->size(), pkt->userdata());
        }

        if (pkt->userdata())
            printf("[%d] Received payload of size %d\n", here_.addr_, pkt->userdata()->size());

    //if the packet was an ack
    } else if (oldHdr->type() == T_ack) {
        //need to find the packet that was acked in our window
        for (int i = 0; i < WINDOW_SIZE; ++i){

            //if the packet is a packet in this element...
            if (window[i] != NULL) {
                hdr_Xyzzy* hdr = hdr_Xyzzy::access(window[i]);

                //and its sequence number is == to the one packet we just got
                if (hdr->seqno() == oldHdr->seqno()){
                    printf("[%d] Got an ack for %d and marking the packet in the buffer.\n", here_.addr_, oldHdr->seqno());
                    // found it, delete the packet
                    Packet::free(window[i]);
                    window[i] = NULL;

                    break;
                //if the packet is less than the cumulative ack that came in
                //on the newest packet we can dump it too while were at it.
                } else if(hdr->seqno() < oldHdr->cumAck()) {
                    printf("[%d] Got a cumAck for %d and marking the packet %d in the buffer.\n", here_.addr_, oldHdr->cumAck(), hdr->seqno());
                    Packet::free(window[i]);
                    window[i] = NULL;
                }
            }
        }
    }

    //free the packet we just received
    //BEWARE THAT THIS FUCTION DOES THIS
    Packet::free(pkt);

}

//This function processes commands to the agent from the TCL script
int XyzzyAgent::command(int argc, const char*const* argv) {

    if (argc == 3 && strcmp(argv[1], "set-multihome-core") == 0) {
        coreTarget = (Classifier *) TclObject::lookup(argv[2]);
        if(coreTarget == NULL)
        {
            return (TCL_ERROR);
        }
        return (TCL_OK);
    } else if (argc == 3 && strcmp(argv[1], "set-primary-destination") == 0) {
        Node* opNode = (Node *) TclObject::lookup(argv[2]);
        if(opNode == NULL)
          {
            return (TCL_ERROR);
          }
        
        // see if the dest is in the list and make it the primary
        if (destList == NULL){
            printf("[%d] Trying to set primary dest when there are no destinations\n", here_.addr_);
            return (TCL_ERROR);
        }

        for(DestNode* current = destList; current != NULL; current = current->next){
           if (current->iNsAddr == opNode->address()){
                primaryDest = current;
                return (TCL_OK);
           }
        }

        return (TCL_ERROR);
        
    } else if (argc == 4 && strcmp(argv[1], "send") == 0) {
        PacketData* d = new PacketData(strlen(argv[3]) + 1);
        strcpy((char*)d->data(), argv[3]);
        sendmsg(atoi(argv[2]), d);
        return TCL_OK;
    } else if (argc == 4 && strcmp(argv[1], "add-multihome-destination") == 0) {
        int iNsAddr = atoi(argv[2]);
        int iNsPort = atoi(argv[3]);
        AddDestination(iNsAddr, iNsPort);
        return (TCL_OK);
    } else if (argc == 5 && strcmp(argv[1], "sendmsg") == 0) {
        PacketData* d = new PacketData(strlen(argv[3]) + 1);
        strcpy((char*)d->data(), argv[3]);
        sendmsg(atoi(argv[2]), d, argv[4]);
        return TCL_OK;
    }else if (argc == 6 && strcmp(argv[1], "add-multihome-interface") == 0) {
        // get interface information and pass it to AddInterface
        int iNsAddr = atoi(argv[2]);
        int iNsPort = atoi(argv[3]);
        NsObject* opTarget = (NsObject *) TclObject::lookup(argv[4]);
        if(opTarget == NULL)
        {
            return (TCL_ERROR);
        }
        NsObject* opLink = (NsObject *) TclObject::lookup(argv[5]);
        if(opLink == NULL)
        {
            return (TCL_ERROR);
        }
        AddInterface(iNsAddr, iNsPort, opTarget, opLink);
        return (TCL_OK);
    }

    return Agent::command(argc, argv);
}

// Add an interface to the interface list
void XyzzyAgent::AddInterface(int iNsAddr, int iNsPort,
        NsObject *opTarget, NsObject *opLink) {
    IfaceNode* iface = new IfaceNode;
    iface->iNsAddr = iNsAddr;
    iface->iNsPort = iNsPort;
    iface->opTarget = opTarget;
    iface->opLink = opLink;

    IfaceNode* head = ifaceList;

    iface->next = head;
    ifaceList = iface;
}

// add a new destination to the destination list
void XyzzyAgent::AddDestination(int iNsAddr, int iNsPort) {
    DestNode* newDest = new DestNode;
    newDest->iNsAddr = iNsAddr;
    newDest->iNsPort = iNsPort;

    // set the primary to the last destination added just in case the user does not set a primary
    primaryDest = newDest;

    DestNode* head = destList;

    newDest->next = head;
    destList = newDest;
}


/* vi: set tabstop=4 softtabstop=4 shiftwidth=4 expandtab : */
