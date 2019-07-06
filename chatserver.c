//c0deless TOR CHAT CONCEPT SERVER

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
#include<sys/resource.h>
#include<poll.h>

//if maxclients is set higher than the resource limit on the number of open filedescriptors
//poll will fail, if we are root we fix it at startup, up to the system wide limit at most,
//if the server does not run the number of clients specified here check /proc/sys/fs/file-max
//if the server is not run as root, set ulimit -n manually to slightly above MAXCLIENTS.
//tested up to 200000 clients, 990 clients on a default ubuntu system when not root.
//the server does not close stdin,stdout,stderr and these use up filedescriptors too.
//also Tor has the same problem as the server but needs more filedescriptors PER CIRCUIT.
//setting it to a few million should be no problem on properly configured modern systems,
//but also for the tor daemon (and/or use multiple tor daemons loadbalancing).

#define MAXCLIENTS 990

//PROTOCOL SPECIFICATION:
//AFTER CONNECT BOTH CLIENT AND SERVER SEND EACH OTHER A FULLY FILLED OUT CHATMESSAGEBLOCK WITH ZEROED MESSAGE FIELD
//USERNAME CHANGES CAN BE DONE WITH ANY CHATMESSAGEBLOCK BUT THE CLIENT CAN SEND A ZERO MESSAGE ONE TO EFFECTUATE IMMEDIATELY
//IN NORMAL USE NO OTHER ZEROED MESSAGE CHATMESSAGEBLOCKS SHOULD GO EITHER WAY
//THE TIMESTAMPS ARE ALWAYS IN UTC/ZULU TIME IN SECONDS AS BY UNIX EPOCH
//CHATMESSAGEBLOCKS CLIENT->SERVER ALWAYS HAVE THE TIMESTAMP FIELD SET TO 4 BYTES ZERO
//THE TIMESTAMP IS IN INTEL HOST BYTE ORDER
//IF EITHER THE CLIENT OR THE SERVER WIPE EXPIRED MESSAGES, THE TIME TO BE USED IS THE TIME SUPPLIED BY THE SERVER.
//CLIENTS SHOULD KEEP A DELTA BETWEEN THEIR OWN CLOCK AND THAT OF THE SERVER TO DETERMINE EXPIRY

struct chatmessageblock{
uint8_t startmarker;         //ALWAYS $FA
unsigned char username[15];  //ZERO TERMINATED STRING IN ZERO TERMINATED FIELD 2-14 BYTES $21-$7E ASCII
uint64_t timestamp;          //ZERO WHEN CLIENT -> SERVER, ZULU/UTC 64 BIT TIMESTAMP IN HOST ORDER WHEN SERVER<CLIENT
unsigned char message[439];  //ZERO TERMINATED STRING IN ZERO TERMINATED FIELD $20-$7E ASCII - MESSAGES ARE CLEANED OF DOUBLE AND TRAILING ZEROES
uint8_t endmarker;           //ALWAYS $FB
}cmb;

//ANY PACKET NOT CONFORMING TO PROTOCOL WILL RESULT IN THE CONNECTION TO BE INSTANTLY DROPPED WITHOUT IN-BAND NOTIFICATION
//BOTH AT CLIENTS AND SERVERS

//WHEN MAKING SIGNIFICANT CHANGES TO THE CHATMESSAGEBLOCK STRUCTURE, STRUCTURE SIZE MUST ALWAYS ALIGN TO 64BIT BOUNDARIES,
//(MULTIPLES OF 8 BYTES) OR SIZEOF() CANNOT BE USED IN SEND AND RECV IN THAT CASE

struct clients{
int fd;                      //FILE DESCRIPTOR OF EACH CLIENT. -1 IF NOT CONNECTED
int cmblength;               //REMAINING BYTES TO BE RECEIVED, NORMALLY THE SIZE OF STRUCT CHATMESSAGEBLOCK, 0 INDICATES READY FOR PARSING
struct chatmessageblock cmb; //THE (PARTIAL) CHATMESSAGEBLOCK THE CLIENT IS RECEIVING
unsigned char username[15];  //THE CLIENTS LAST SEEN USERNAME, A DIFFERENT ONE IN A COMPLETED CHATMESSAGEBLOCK INDICATES A USERNAME CHANGE
time_t lastseen;             //CURRENTLY ONLY USED TO PURGE NEW CONNECTIONS THAT NEVER SENT ANY DATA AFTER A FEW SECONDS
}cl[MAXCLIENTS];             //THIS STRUCT HOLDS ONE ELEMENT FOR EACH POSSIBLE CLIENT

//RESERVED USERNAMES: "SERVER", "LIST" CLIENTS USING THOSE, WILL BE INSTANTLY DROPPED AS WELL


//ADDITIONAL STRUCT THAT HAS 1 ENTRY FOR ALL CLIENTS
//NOT SURE IF THE struct pollfd.event AND struct pollfd.revent CAN BE INTEGRATED WITH THE CLIENTS ARRAY. THE FILEDESCRIPTOR FIELD IS THE SAME
struct pollfd pl[MAXCLIENTS+1];
//ACCORDING TO THE DOCUMENTATION, THE -NUMBER- OF FILEDESCRIPTORS + 1, NOT THE HIGHEST VALUE
nfds_t nfds;
//THE MAIN SOCKET, USED TO ACCEPT INCOMING CONNECTIONS, WHICH THEN GET THEIR OWN FILEDESCRIPTORS IN THE CLIENT AND POLL TABLES
int sockfd;
//VARIOUS
int n;
//SETSOCKOPT WANTS A POINTER TO INTEGER, SO WE NEED AN INTEGER TO POINT TO
int true;
//THE LISTEN ADDRESS AND PORT GO IN THIS STRUCT
struct sockaddr_in saddr;
//MAINLY USED TO SEE HOW MUCH BYTES ARE RECEIVED
int ret;
//DOING SOME CALCULATIONS DIRECTLY WITH SIZEOF GAVE SOME PROBLEMS AS SIZEOF IS NOT ALWAYS INT
int cmbsize;
//CAST USED TO CALCULATE OFFSET WITHIN THE STRUCT RCMB FOR PARTIAL RECEIVE RECV
uint8_t *cast;
//RESOURCE LIMIT STRUCT TO RAISE MAXIMUM FILEDESCRIPTORS IF POSSIBLE
struct rlimit rlim;

void setuplistener(){
while(1){
sockfd=socket(PF_INET,SOCK_STREAM,IPPROTO_TCP);
if(sockfd==-1){printf("SOCKET CREATION FAILED\n");sleep(1);continue;};
true=1;
setsockopt(sockfd,SOL_SOCKET,SO_REUSEADDR,&true,sizeof(true));
setsockopt(sockfd,SOL_SOCKET,SO_KEEPALIVE,&true,sizeof(true));
saddr.sin_family=AF_INET;
saddr.sin_port=htons(2081);
inet_aton("127.0.0.1",&saddr.sin_addr);
if(bind(sockfd,(struct sockaddr*)&saddr,sizeof(saddr))!=0){printf("SOCKET BIND FAILED\n");close(sockfd);sleep(1);continue;};
if(listen(sockfd,1024)!=0){printf("SOCKET LISTEN FAILED\n");close(sockfd);sleep(1);continue;};
printf("SOCKET LISTEN SUCCESS\n");
break;
};
};//setuplistener

void leftmessage(int s){
cmb.startmarker=0xFA;
strncpy(cmb.username,"SERVER",sizeof(cmb.username)-1);
cmb.timestamp=time(NULL);
strncpy(cmb.message,cl[s].username,sizeof(cmb.username)-1);
strcat(cmb.message," HAS LEFT THE CHAT");
cmb.endmarker=0xFB;
int d;
for(d=0;d<MAXCLIENTS;d++)if(cl[d].username[0]!=0){
cl[s].cmb.timestamp=time(NULL);
//don't check if client is alive, calling purgeclient from here could cause a loop
send(cl[d].fd,&cmb,cmbsize,MSG_NOSIGNAL);
};//mainloop done, clear message
bzero(&cmb,sizeof(cmb));
};//leftmessage

void purgeclient(int c){
printf("PURGE SLOT %d FD %d\n",c,cl[c].fd);
if(cl[c].username[0]!=0)leftmessage(c);
close(cl[c].fd);
cl[c].fd=-1;
pl[c+1].fd=-1;
cl[c].cmblength=cmbsize;
bzero(&cl[c].cmb,sizeof(cmb));
bzero(&cl[c].username,sizeof(cl[c].username));
cl[c].lastseen=0;
};//purgeclient

void welcomemessage(int d){
cmb.startmarker=0xFA;
strncpy(cmb.username,"SERVER",sizeof(cmb.username)-1);
cmb.timestamp=time(NULL);
strncpy(cmb.message,"WELCOME TO PRIVATE ALPS CHAT SERVER - TYPE :HELP FOR HELP",sizeof(cmb.message)-1);
cmb.endmarker=0xFB;
if(send(cl[d].fd,&cmb,cmbsize,MSG_NOSIGNAL)==-1)purgeclient(d);
bzero(&cmb,sizeof(cmb));
};//welcomemessage

void helpmessage(int d){
cmb.startmarker=0xFA;
strncpy(cmb.username,"SERVER",sizeof(cmb.username)-1);
cmb.timestamp=time(NULL);
strncpy(cmb.message,"COMMANDS - :HELP :LIST :QUIT",sizeof(cmb.message)-1);
cmb.endmarker=0xFB;
if(send(cl[d].fd,&cmb,cmbsize,MSG_NOSIGNAL)==-1)purgeclient(d);
bzero(&cmb,sizeof(cmb));
};//helpmessage

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

void joinedmessage(int s){
cmb.startmarker=0xFA;
strncpy(cmb.username,"SERVER",sizeof(cmb.username)-1);
cmb.timestamp=time(NULL);
strncpy(cmb.message,cl[s].cmb.username,sizeof(cl[s].cmb.username)-1);
strcat(cmb.message," HAS JOINED THE CHAT");
cmb.endmarker=0xFB;
int d;
for(d=0;d<MAXCLIENTS;d++)if(cl[d].username[0]!=0){
cl[s].cmb.timestamp=time(NULL);
if(send(cl[d].fd,&cmb,cmbsize,MSG_NOSIGNAL)==-1)purgeclient(d);
};//mainloop done, clear message
bzero(&cmb,sizeof(cmb));
};//joinedmessage

void nickchangedmessage(int s){
cmb.startmarker=0xFA;
strncpy(cmb.username,"SERVER",sizeof(cmb.username)-1);
cmb.timestamp=time(NULL);
strncpy(cmb.message,cl[s].username,sizeof(cl[s].username)-1);
strncat(cmb.message," CHANGED NAME TO <",sizeof(cmb.message)-1);
strncat(cmb.message,cl[s].cmb.username,sizeof(cl[s].cmb.username)-1);
strcat(cmb.message,">");
cmb.endmarker=0xFB;
int d;
for(d=0;d<MAXCLIENTS;d++)if(cl[d].username[0]!=0){
cl[s].cmb.timestamp=time(NULL);
if(send(cl[d].fd,&cmb,cmbsize,MSG_NOSIGNAL)==-1)purgeclient(d);
}//mainloop done, clear message
bzero(&cmb,sizeof(cmb));
};//nickchangedmessage

void sendmessagetoall(int s){
int d;
cl[s].cmb.timestamp=time(NULL);//do this before the loop so all clients receive the same server timestamp for the same message
for(d=0;d<MAXCLIENTS;d++)if((cl[d].fd!=-1)&&(cl[d].username[0]!=0)){
printf("SENDTOALL: SOURCE: %d FD %d DEST: %d %d\n",s,cl[s].fd,d,cl[d].fd);
if(send(cl[d].fd,&cl[s].cmb,cmbsize,MSG_NOSIGNAL)==-1)purgeclient(d);
};//mainloop done, clear message
};//sendmessagetoall

void cleararray(){
bzero(&cl,sizeof(cl));
for(n=0;n<MAXCLIENTS;n++){
cl[n].fd=-1;
pl[n+1].fd=-1;
pl[n+1].events=POLLIN;
cl[n].cmblength=cmbsize;
};
};//cleararray

void checkusername(int s){
//PROTOCOL SPECIFICATION: 2-14 BYTES ZERO TERMINATED $21 till $7E ASCII, BOTH STRING (IF SHORTER) AND FIELD TERMINATOR ZERO
int x;
for(x=0;((x<sizeof(cl[s].cmb.username)-1)&&(cl[s].cmb.username[x]!=0));x++){
if((cl[s].cmb.username[x]<0x21)||(cl[s].cmb.username[x]>0x7E)){bzero(&cl[s].cmb.username,sizeof(cl[s].cmb.username));break;};
};
//X NOW IS THE STRINGLENGTH, ALSO CHECK FOR PROTOCOL VIOLATION
if((x<2)||(cl[n].cmb.username[sizeof(cl[n].cmb.username)-1]!=0)||(strncmp(cl[n].cmb.username,"SERVER",sizeof(cl[n].cmb.username))==0)||(strncmp(cl[n].cmb.username,"LIST",sizeof(cl[n].cmb.username))==0))bzero(&cl[s].cmb.username,sizeof(cl[s].cmb.username));
};//checkusername

void listmessage(int d){
int u;
int o;
int m;
cmb.startmarker=0xFA;
strncpy(cmb.username,"LIST",sizeof(cmb.username)-1);
cmb.timestamp=time(NULL);
bzero(&cmb.message,sizeof(cmb.message));
cmb.endmarker=0xFB;
o=0;
for(u=0;u<MAXCLIENTS;u++){
if((cl[u].username[0]!=0)&&(o<(sizeof(cmb.message)-sizeof(cmb.username)))){
for(m=0;((cl[u].username[m]!=0)&&(m<sizeof(cl[u].username)));m++){cmb.message[o]=cl[u].username[m];o++;};
cmb.message[o]=0x20;o++;
};//username populated
if(cmb.message[0]!=0)if((u==(MAXCLIENTS-1))||(o>=(sizeof(cmb.message)-sizeof(cmb.username)))){
if(o>0)o--;//remove trailing space
cmb.message[o]=0x00;o++;//terminate message
if(cl[d].fd!=-1)if(send(cl[d].fd,&cmb,cmbsize,MSG_NOSIGNAL)==-1)purgeclient(d);
bzero(&cmb.message,sizeof(cmb.message));
o=0;
};//packet has content, send and zero out
};//foreach user
bzero(&cmb,sizeof(cmb));
};//listmessage

int main(void){
rlim.rlim_cur=MAXCLIENTS+10;
rlim.rlim_max=MAXCLIENTS+10;
setrlimit(RLIMIT_NOFILE,&rlim);
setuplistener();
cmbsize=sizeof(struct chatmessageblock);
cleararray();
nfds=MAXCLIENTS+1;
while(1){
pl[0].fd=sockfd;
pl[0].events=POLLIN;
for(n=0;n<(MAXCLIENTS);n++)pl[n+1].fd=cl[n].fd;
poll(pl,nfds,5000);
if(pl[0].revents!=0){//read on sockfd indicates new connection
for(n=0;((cl[n].fd!=-1)&&(n<MAXCLIENTS));n++);//determine lowest free slot
if(n<(MAXCLIENTS-1)){
cl[n].fd=accept(sockfd,NULL,NULL);
if(cl[n].fd!=-1){
printf("ADDED NEW CLIENT SLOT %d FD %d\n",n,cl[n].fd);
setsockopt(cl[n].fd,SOL_SOCKET,SO_KEEPALIVE,&true,sizeof(true));
fcntl(cl[n].fd,F_SETFL,fcntl(cl[n].fd,F_GETFL,0)|O_NONBLOCK);
pl[n+1].fd=cl[n].fd;
cl[n].cmblength=cmbsize;
bzero(&cl[n].cmb,sizeof(cl[n].cmb));
bzero(&cl[n].username,sizeof(cl[n].username));
cl[n].lastseen=time(NULL);
};//accept success, initialize array entry
};//maxclients not reached
if(n==(MAXCLIENTS-1)){
cl[n].fd=accept(sockfd,NULL,NULL);
if(cl[n].fd!=-1){
printf("SERVER FULL ERROR WHILE ADDING NEW CLIENT SLOT %d FD %d\n",n,cl[n].fd);
//we don't send anything, as we can't wait for the client to identify itself, nor inform the outside world about what we are.
//we just drop the connections, legitimate clients will keep reconnecting, attackers won't know what protocol it is.
close(cl[n].fd);//instant drop, same as with any received initial packet not conforming to protocol specifications.
cl[n].fd=-1;
};//accept success, but server full, kick
};//server full
};//new client

//FOR ALL SESSION SLOTS LOOP
for(n=0;n<MAXCLIENTS;n++){

//APPEND INGRESS CHATMESSAGEBLOCK FOR SESSION SLOT N
if((cl[n].fd!=-1)&&(pl[n+1].revents!=0)){
printf("REMAINING READ FOR SLOT %d FD %d: %d / %d OFFSET %d\n",n,cl[n].fd,cl[n].cmblength,cmbsize,(cmbsize-cl[n].cmblength));
cast=(uint8_t*)&cl[n].cmb;
cast=cast+cmbsize-cl[n].cmblength;
ret=recv(cl[n].fd,cast,cl[n].cmblength,0);
//clients can also disconnect by sending a length 0 CMB, they will receive their own parted message in that case
if(ret<1){printf("CLIENT DISCONNECTED SLOT %d FD %d\n",n,cl[n].fd);purgeclient(n);continue;};
if(ret>0){
printf("RECEIVED %d BYTES ON SLOT %d FD %d\n",ret,n,cl[n].fd);
if(cl[n].cmb.startmarker!=0xFA){printf("PROTOCOL SYNC ERROR SLOT %d FD %d\n",n,cl[n].fd);purgeclient(n);continue;};
cl[n].cmblength=cl[n].cmblength-ret;
};//more than 0 bytes
};//append ingress message buffers

//PROCESS COMPLETED INGRESS CHATMESSAGEBLOCK FOR SESSION SLOT N
if((cl[n].fd!=-1)&&(cl[n].cmblength==0)){
//drop any connection which sends a packet that doesn't fully conform to the standard instantly and without notification
if(cl[n].cmb.startmarker!=0xFA){printf("PROTOCOL STARTMARKER FORMAT ERROR SLOT %d FD %d\n",n,cl[n].fd);purgeclient(n);continue;};
if(cl[n].cmb.endmarker!=0xFB){printf("PROTOCOL ENDMARKER FORMAT ERROR SLOT %d FD %d\n",n,cl[n].fd);purgeclient(n);continue;};
if(cl[n].cmb.username[sizeof(cl[n].cmb.username)-1]!=0){printf("PROTOCOL USERNAME FORMAT ERROR SLOT %d FD %d\n",n,cl[n].fd);purgeclient(n);continue;};
checkusername(n);//if the username contains any illegal character sequence, zero it out, which will cause the next check to purge the client
if(cl[n].cmb.username[0]==0){printf("PROTOCOL USERNAME INVALID ERROR SLOT %d FD %d\n",n,cl[n].fd);purgeclient(n);continue;};
if(cl[n].cmb.timestamp!=0){printf("PROTOCOL TIMESTAMP FORMAT ERROR SLOT %d FD %d\n",n,cl[n].fd);purgeclient(n);continue;};
if(cl[n].cmb.message[sizeof(cl[n].cmb.message)-1]!=0){printf("PROTOCOL MESSAGE FORMAT ERROR SLOT %d FD %d\n",n,cl[n].fd);purgeclient(n);continue;};
//remove leading and trailing spaces, double spaces, and illegal characters (7 bit ASCII only).
if(cl[n].cmb.message[0]!=0)cleanmessage((unsigned char*)&cl[n].cmb.message);
//if the username in the client table isn't the same as the username in the packet, it's either a new client or a nick change
if((cl[n].cmb.username[0]!=0)&&(strncmp(cl[n].username,cl[n].cmb.username,sizeof(cl[n].username)-1)!=0)){
if(cl[n].username[0]==0){welcomemessage(n);joinedmessage(n);};
if((cl[n].username[0]!=0)&&(strncmp(cl[n].cmb.username,cl[n].username,sizeof(cl[n].username)-1)!=0))nickchangedmessage(n);
strncpy(cl[n].username,cl[n].cmb.username,sizeof(cl[n].username)-1);
};//handle joins and nick changes
//handle commands
if(strcmp(cl[n].cmb.message,":QUIT")==0){purgeclient(n);cl[n].cmb.message[0]=0;};
if(strcmp(cl[n].cmb.message,":HELP")==0){helpmessage(n);cl[n].cmb.message[0]=0;};
if(strcmp(cl[n].cmb.message,":LIST")==0){listmessage(n);cl[n].cmb.message[0]=0;};
//send it to all connected clients if it's not an initial empty chatmessageblock with just a username
if(cl[n].cmb.message[0]!=0)sendmessagetoall(n);
//wipe all traces of the packet message content.
cl[n].cmblength=cmbsize;
bzero(&cl[n].cmb,sizeof(cl[n].cmb));
};//handle completed ingress messages

//PURGE SESSIONS THAT DID NOT SEND ANY DATA WITHIN 30 SECONDS AFTER CONNECTION
if((cl[n].fd!=-1)&&(cl[n].username[0]==0)&&(cl[n].lastseen<(time(NULL)-30))){printf("PROTOCOL CLIENT EXPIRED SLOT %d FD %d\n",n,cl[n].fd);purgeclient(n);};
};//for session slot receive loop

};//while 1
};//main
