﻿#include <iostream>
#include "fpnn.h"

using namespace std;
using namespace fpnn;

class ExampleQuestProcessor : public IQuestProcessor
{
    QuestProcessorClassPrivateFields(ExampleQuestProcessor)

public:
    FPAnswerPtr duplexQuest(const FPReaderPtr args, const FPQuestPtr quest, const ConnectionInfo& ci)
    {
        int value = (int)args->wantInt("int");
        cout<<"Receive server push. value of key 'int' is "<<value<<endl;
        return FPAWriter::emptyAnswer(quest);
    }

    ExampleQuestProcessor()
    {
        registerMethod("duplex quest", &ExampleQuestProcessor::duplexQuest);
    }

    QuestProcessorClassBasicPublicFuncs
};

int main(int argc, const char** argv)
{
    if (argc != 2)
    {
        cout<<"Usage: "<<argv[0]<<" <endpoint>"<<endl;
        return 0;
    }

    WSADATA wsaData;
    int iResult = WSAStartup(MAKEWORD(2,2), &wsaData);
    if (iResult != 0)
    {
        cout<<"WSAStartup failed: "<<iResult<<endl;
        return 1;
    }

    TCPClientPtr client = TCPClient::createClient(argv[1]);
    client->setQuestProcessor(std::make_shared<ExampleQuestProcessor>());


    FPQWriter qw(1, "duplex demo");
    qw.param("duplex method", "duplex quest");
    FPQuestPtr quest = qw.take();

    FPAnswerPtr answer = client->sendQuest(quest);
    FPAReader ar(answer);
    if (ar.status() == 0)
        cout<<"Received answer of quest."<<endl;
    else
        cout<<"Received error answer of quest. code is "<<ar.wantInt("code")<<endl;

    return 0;
}