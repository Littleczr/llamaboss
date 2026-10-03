// chat_folders_tests.cpp -- standalone checks for chat_folders.h.
// Build: cl /std:c++17 /EHsc chat_folders_tests.cpp   (or g++ -std=c++17)

#include "chat_folders.h"
#include <cstdio>
using namespace chat_folders;
int fails=0;
#define EQ(a,b) do{auto _a=(a);auto _b=std::string(b); if(_a!=_b){printf("FAIL %s: got [%s] want [%s]\n",#a,_a.c_str(),_b.c_str());fails++;}}while(0)
#define T(x) do{if(!(x)){printf("FAIL %s\n",#x);fails++;}}while(0)
int main(){
 EQ(MakeTitleSlug("Fix the invoice parser (again!)"),"fix-the-invoice-parser-again");
 EQ(MakeTitleSlug("Untitled conversation"),"");
 EQ(MakeTitleSlug("¿Qué pasa?"),"que-pasa");
 EQ(MakeTitleSlug("   "),"");
 EQ(MakeTitleSlug("Sharing the latest llamaboss source code for your reference wondering about naming"),"sharing-the-latest-llamaboss-source-code");
 T(MakeTitleSlug("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa").size()==40);
 EQ(BuildChatFolderName("2026-09-26","Fix invoice parser","1ed3d3c2"),"2026-09-26_fix-invoice-parser_1ed3d3c2");
 EQ(BuildChatFolderName("2026-09-26","","1ed3d3c2"),"2026-09-26_1ed3d3c2");
 EQ(ChatIdFromConversationStem("chat_1ed3d3c2"),"1ed3d3c2");
 EQ(ChatIdFromConversationStem("chat_1ED3D3C2"),"1ed3d3c2");
 T(ChatIdFromConversationStem("My notes").size()==8);
 EQ(ChatIdFromConversationStem("My notes"),ChatIdFromConversationStem("my notes"));
 EQ(ChatIdFromFolderName("2026-09-26_fix-invoice-parser_1ed3d3c2"),"1ed3d3c2");
 EQ(ChatIdFromFolderName("2026-09-26_1ed3d3c2"),"1ed3d3c2");
 EQ(ChatIdFromFolderName("chat_1ed3d3c2"),"1ed3d3c2");
 EQ(ChatIdFromFolderName("2026-09-26_notes"),"");
 EQ(ChatIdFromFolderName("Sources"),"");
 EQ(ChatIdFromFolderName("2026-09-26_x_1ed3d3c2-1111-2222-3333-444455556666"),"1ed3d3c2-1111-2222-3333-444455556666");
 // round trip: slug that ends with 8 hex chars
 EQ(ChatIdFromFolderName(BuildChatFolderName("2026-01-01","deadbeef","1ed3d3c2")),"1ed3d3c2");
 EQ(ChatFolderFromWorkspaceCwd("C:\\Users\\Cesar\\LlamaBoss\\Chats\\2026-09-26_fix_1ed3d3c2\\Workspace"),"C:\\Users\\Cesar\\LlamaBoss\\Chats\\2026-09-26_fix_1ed3d3c2");
 EQ(ChatFolderFromWorkspaceCwd("C:\\Users\\Cesar\\LlamaBoss\\Chats\\2026-09-26_fix_1ed3d3c2\\Workspace\\"),"C:\\Users\\Cesar\\LlamaBoss\\Chats\\2026-09-26_fix_1ed3d3c2");
 EQ(ChatFolderFromWorkspaceCwd("C:\\Users\\Cesar\\LlamaBoss\\Workflows\\chat_1ed3d3c2\\Workspace"),"C:\\Users\\Cesar\\LlamaBoss\\Workflows\\chat_1ed3d3c2");
 EQ(ChatFolderFromWorkspaceCwd("C:\\Users\\Cesar\\LlamaBoss\\Workspace"),"");
 EQ(ChatFolderFromWorkspaceCwd("C:\\Proj\\Workflows\\helper\\Workspace"),"");
 EQ(ChatFolderFromWorkspaceCwd("C:\\x\\Other\\2026-09-26_fix_1ed3d3c2\\Workspace"),"");
 EQ(ChatFolderFromWorkspaceCwd("C:\\x\\chats\\2026-09-26_fix_1ed3d3c2\\workspace"),"C:\\x\\chats\\2026-09-26_fix_1ed3d3c2");
 EQ(MakeTitleSlug("Niño año Ñandú"),"nino-ano-nandu");
 printf(fails?"%d failures\n":"all passed\n",fails); return fails;
}
