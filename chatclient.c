//c0deless TOR CHAT CONCEPT CLIENT


#include<unistd.h>
#include<stdio.h>
#include<stdint.h>
#include<string.h>
#include<arpa/inet.h>
#include<sys/types.h>
#include<sys/socket.h>
#include<sys/time.h>
#include<sys/types.h>
#include<time.h>
#include<fcntl.h>
#include<stdlib.h>
#include<libgen.h>

struct chatmessageblock{
uint8_t startmarker;
char username[15];
uint64_t timestamp;
char message[439];
uint8_t endmarker;
}rcmb,scmb;

struct socks4areq{
uint8_t ver;
uint8_t cmd;
uint16_t port;
uint32_t ip;
uint8_t user;
uint8_t onion[63];
}s4ar;

struct socks4astatus{
uint8_t zero;
uint8_t status;
uint16_t port;
uint32_t ip;
}s4as;


int sockfd;
int true;
struct sockaddr_in saddr;
struct timeval tv;
fd_set fds;
int ret;
int cmbsize;
struct tm *ts;
int rcmblength;
uint8_t *cast;
int maxfd;
int n;

int torconnect(){
s4ar.ver=0x04;
s4ar.cmd=0x01;
s4ar.port=htons(2081);
s4ar.ip=inet_addr("0.0.0.1"); //the documentation actually is unclear about this being in network byte order, it is for all other socks versions.
s4ar.user=0x00;
strncpy(s4ar.onion,"Hidden service adresse.onion",sizeof(s4ar.onion)-1);
s4ar.onion[sizeof(s4ar.onion)-1]=0;
saddr.sin_family=AF_INET;
saddr.sin_port=htons(9050);
inet_aton("127.0.0.1",&saddr.sin_addr);
bzero(&scmb.message,sizeof(scmb.message));
while(1){
printf("CONNECTING\n");
sockfd=socket(PF_INET,SOCK_STREAM,IPPROTO_TCP);
if(sockfd==-1){printf("SOCKET SETUP ERROR\n");sleep(1);continue;};
setsockopt(sockfd,SOL_SOCKET,SO_KEEPALIVE,&true,sizeof(true));
if(connect(sockfd,(struct sockaddr*)&saddr,sizeof(saddr))!=0){close(sockfd);printf("CONNECTING TOR SOCKS4A CONNECT ERROR\n");sleep(1);continue;};
if(send(sockfd,&s4ar,sizeof(s4ar),MSG_NOSIGNAL)==-1){close(sockfd);printf("CONNECTING TOR SOCKS4A DISCONNECTED SENDING SOCKS4A REQUEST ERROR\n");sleep(1);continue;};
tv.tv_sec=60;
tv.tv_usec=0;
FD_SET(sockfd,&fds);
if(select(sockfd+1,&fds,NULL,NULL,&tv)<1){close(sockfd);printf("CONNECTING TOR SOCKS4A REQUEST TIMEOUT ON HOST REPLY ERROR\n");sleep(1);continue;};
if(recv(sockfd,&s4as,sizeof(s4as),0)<sizeof(s4as)){close(sockfd);printf("CONNECTING TOR SOCKS4A DISCONNECTED ERROR\n");sleep(1);continue;};
if((s4as.zero!=0x00)||(s4as.status!=0x5A)){close(sockfd);printf("CONNECTING TOR SOCKS4A REQUEST DENIED ERROR\n");sleep(1);continue;};
printf("CONNECTING TOR SOCKS4A HOST REQUEST ACCEPTED\n");
if(send(sockfd,&scmb,cmbsize,MSG_NOSIGNAL)<1){close(sockfd);printf("CONNECTING DISCONNECTED ON INITIAL CHATMESSAGEBLOCK ERROR\n");sleep(1);continue;};
printf("CONNECTING COMPLETED\n");
break;
};//while 1
rcmblength=cmbsize;
bzero(&rcmb,cmbsize);
return(0);
};//torconnect

void cleanmessage(unsigned char *st){
unsigned int ix;
unsigned int ox;
//STRING CLEANUP CONTROL CHARS + HIGHER ASCII
for(ix=0;st[ix]!=0;ix++)if((st[ix]<0x20)||(st[ix]>0x7E))st[ix]=0x20;
//STRING CLEANUP DOUBLE, LEADING AND TRAILING SPACE - RETURNS LENGTH
while(st[0]==0x20)for(ox=0;st[ox]!=0;ox++)st[ox]=st[ox+1];
for(ix=0;st[ix]!=0;ix++)while((st[ix]==0x20)&&(st[ix+1]==0x20))for(ox=ix;st[ox]!=0;ox++)st[ox]=st[ox+1];
for(ox=ix;((ox>0)&&(st[ox-1]==0x20));ox--)st[ox-1]=0;
};//cleanmessage

int main(int argc,unsigned char *argv[]){
if(argc<2)strncpy(scmb.username,"USERNAME",sizeof(scmb.username)-1);
if(argc>2){printf("USAGE: %s [NICKNAME]\n",basename(argv[0]));exit(0);};
if(argc==2)strncpy(scmb.username,argv[1],sizeof(scmb.username)-1);
for(n=0;((n<sizeof(scmb.username)-1)&&(scmb.username[n]!=0));n++)if((scmb.username[n]<0x21)||(scmb.username[n]>0x7E)){printf("INVALID USERNAME\n");exit(0);};
if(n<2){printf("USERNAME TOO SHORT\n");exit(0);};
if((strncmp(scmb.username,"SERVER",sizeof(scmb.username))==0)||(strncmp(scmb.username,"LIST",sizeof(scmb.username))==0)){printf("RESERVED USERNAME\n");exit(0);};
printf("CURRENT USERNAME: %s\n",scmb.username);
true=1;
scmb.startmarker=0xFA;
scmb.timestamp=0;
scmb.endmarker=0xFB;
cmbsize=sizeof(struct chatmessageblock);
torconnect();
while(1){
maxfd=STDIN_FILENO;
FD_ZERO(&fds);
FD_SET(STDIN_FILENO,&fds);
FD_SET(sockfd,&fds);
if(maxfd<sockfd)maxfd=sockfd;
tv.tv_sec=10;
tv.tv_usec=0;
select(maxfd+1,&fds,NULL,NULL,&tv);
if(FD_ISSET(sockfd,&fds)){
cast=(uint8_t*)&rcmb;
cast=cast+(cmbsize-rcmblength);
ret=recv(sockfd,cast,rcmblength,0);
if(ret<1){close(sockfd);printf("LOST CONNECTION\n");torconnect();continue;};
if(ret>0){
if(rcmb.startmarker!=0xFA){close(sockfd);printf("PROTOCOL STARTMARKER FORMAT ERROR\n");torconnect();continue;};
rcmblength=rcmblength-ret;
};
};//data received on sockfd
if(FD_ISSET(STDIN_FILENO,&fds)){
ret=read(STDIN_FILENO,scmb.message,sizeof(scmb.message)-1);
scmb.message[sizeof(scmb.message)-1]=0;
cleanmessage((unsigned char*)&scmb.message);
if(scmb.message[0]!=0)send(sockfd,&scmb,cmbsize,MSG_NOSIGNAL);
bzero(&scmb.message,sizeof(scmb.message));
};
if((rcmb.startmarker==0xFA)&&(rcmblength==0)){
if(rcmb.username[sizeof(rcmb.username)-1]!=0){close(sockfd);printf("PROTOCOL USERNAME FORMAT ERROR\n");torconnect();continue;};
if(rcmb.message[sizeof(rcmb.message)-1]!=0){close(sockfd);printf("PROTOCOL MESSAGE FORMAT ERROR\n");torconnect();continue;};
if(rcmb.endmarker!=0xFB){close(sockfd);printf("PROTOCOL ENDMARKER FORMAT ERROR\n");torconnect();continue;};
ts=gmtime((time_t *)&rcmb.timestamp);
printf("%02d:%02d:%02dZ <%s> %s\n",ts->tm_hour,ts->tm_min,ts->tm_sec,rcmb.username,rcmb.message);
bzero(&rcmb,cmbsize);
rcmblength=cmbsize;
};//full packet
};//while 1
};//main
