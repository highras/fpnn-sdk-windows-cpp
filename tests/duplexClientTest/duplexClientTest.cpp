﻿#include <iostream>
#include <vector>
#include <thread>
#include <assert.h>
#include "TCPClient.h"
#include "UDPClient.h"
#include "CommandLineUtil.h"
#include "IQuestProcessor.h"

using namespace std;
using namespace fpnn;


class QuestProcessor: public IQuestProcessor
{
    QuestProcessorClassPrivateFields(QuestProcessor)
public:
    virtual void connected(const ConnectionInfo&, bool connected) { cout<<"client connect "<<(connected ? "successful" : "failed")<<"."<<endl; }
    virtual void connectionWillClose(const ConnectionInfo&, bool closeByError)
    {
        if (!closeByError)
            cout<<"client close event processed."<<endl;
        else
            cout<<"client error event processed."<<endl;
    }

    FPAnswerPtr serverQuest(const FPReaderPtr args, const FPQuestPtr quest, const ConnectionInfo& ci)
    {
        cout<<"Tow way method demo is called. socket: "<<ci.socket<<", address: "<<ci.ip<<":"<<ci.port<<endl;
        cout<<"quest data: "<<(quest->json())<<endl;

        FPAnswerPtr ans = sendQuest(FPQWriter::emptyQuest("httpDemo"));
        cout<<" Http Demo ans: "<<ans->json()<<endl;
        //std::string* data = new std::string("tow way quest answer");
        return FPAWriter(2, quest)("Simple","one")("duplex", 2);
    }

    QuestProcessor()
    {
        registerMethod("duplex quest", &QuestProcessor::serverQuest);
    }

    QuestProcessorClassBasicPublicFuncs
};

const char* methodNames[] = {"one way demo", "tow way demo", "unkonwn method", "advance answer demo", "delay answer demo"};

FPQuestPtr QWriter(const char* method, bool oneway, FPMessage::FP_Pack_Type def_ptype){
    FPQWriter qw(6,method, oneway, def_ptype);
    qw.param("quest", "one");
    qw.param("int", 2); 
    qw.param("double", 3.3);
    qw.param("boolean", true);
    qw.paramArray("ARRAY",2);
    qw.param("first_vec");
    qw.param(4);
    qw.paramMap("MAP",5);
    qw.param("map1","first_map");
    qw.param("map2",true);
    qw.param("map3",5);
    qw.param("map4",5.7);
    qw.param("map5","中文");
    return qw.take();
}

int sendCount = 0;
static LARGE_INTEGER gc_frequency;

void sProcess(const char* ip, int port){
    std::shared_ptr<Client> client;
    if (CommandLineParser::exist("udp"))
        client = Client::createUDPClient(ip, port);
    else
        client = Client::createTCPClient(ip, port);

    client->setQuestProcessor(std::make_shared<QuestProcessor>());
    int64_t start = slack_real_msec();
    int64_t resp = 0;
    for (int i = 1; i<=sendCount; i++)
    {
        FPQuestPtr quest = QWriter((i%2) ? "duplex demo" : "delay duplex demo", false, FPMessage::FP_PACK_MSGPACK);
        LARGE_INTEGER start_time, stop_time;
        QueryPerformanceCounter(&start_time);
        try{
            FPAnswerPtr answer = client->sendQuest(quest);
            if (quest->isTwoWay()){
                assert(quest->seqNum() == answer->seqNum());
                QueryPerformanceCounter(&stop_time);
                int64_t diff = (stop_time.QuadPart - start_time.QuadPart) * 1000 * 1000 / gc_frequency.QuadPart;
                resp += diff;
            }
        }
        catch (...)
        {
            cout<<"error occurred when sending"<<endl;
        }

        if(i % 10000 == 0){
            int64_t end = slack_real_msec();
            cout<<"Speed:"<<(10000*1000/(end-start))<<" Response Time:"<<resp/10000<<endl;
            start = end;
            resp = 0;
        }

        Sleep(((i%2) ? 1 : 3) * 1000);
    }
    client->close();
}

int main(int argc, char* argv[])
{
    CommandLineParser::init(argc, argv);
    std::vector<std::string> mainParams = CommandLineParser::getRestParams();

    if (mainParams.size() != 4)
    {
        cout<<"Usage: "<<argv[0]<<" ip port threadNum sendCount [-udp]"<<endl;
        return 0;
    }

    WSADATA wsaData;
    int iResult = WSAStartup(MAKEWORD(2,2), &wsaData);
    if (iResult != 0)
    {
        cout<<"WSAStartup failed: "<<iResult<<endl;
        return 1;
    }

    QueryPerformanceFrequency(&gc_frequency);

    // ClientEngine::configQuestProcessThreadPool(2, 1, 2, 4, 10000);

    int thread_num = atoi(mainParams[2].c_str());
    sendCount = atoi(mainParams[3].c_str());
    vector<thread> threads;
    for(int i=0;i<thread_num;++i){
        threads.push_back(thread(sProcess, mainParams[0].c_str(), atoi(mainParams[1].c_str())));
    }   

    for(unsigned int i=0; i<threads.size();++i){
        threads[i].join();
    } 

    WSACleanup();
    return 0;
}

