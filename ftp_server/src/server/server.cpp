#include "server.h"


volatile sig_atomic_t running=1;
int g_server_fd=-1;


int main()
{
    signal(SIGINT,sigint_handler);
    signal(SIGPIPE,SIG_IGN);//忽略SIGPIPE:客户端中断数据传输时服务器不崩溃,否则无法断点续传
    int server_fd=create_server_socket(SERVER_PORT);//控制连接监听柄
    g_server_fd=server_fd;
    cout << "[SERVER] Listening on 2100..." << endl;

while(running){

    int client_fd=accept(server_fd,nullptr,nullptr);

    // accept失败
    if(client_fd<0){
        if(!running){
            break;
        }

        cout<<"[SERVER] accept failed"<<strerror(errno)<<endl;
        continue;
    }

    thread t(client_handler, client_fd);
    t.detach();
}
    cout<<"[SERVER] shutting down..."<<endl;
    return 0;
}

void sigint_handler(int){
    running=0;
    if(g_server_fd>=0){
        close(g_server_fd);
    }
}

//客户端会话线程handler
void client_handler(int client_fd)
{
    ftp_session sess;
    memset(&sess, 0, sizeof(sess));
    sess.data_fd=-1;
    sess.data_listen_fd=-1;

    sess.ctrl_fd=client_fd;

    send_response(client_fd,"220 FTP server ready");

    char buf[BUF_SIZE];

    while(true)
    {
        memset(buf, 0, sizeof(buf));
        int n=recv(client_fd, buf, sizeof(buf), 0);


        if(n<=0){
            break;
        }
        string cmd(buf);
        while(!cmd.empty()&&(cmd.back()=='\r'||cmd.back()=='\n'))
        {
            cmd.pop_back();
        }

        if(handle_command(&sess,cmd)){
            break;
        }
    }

    close(client_fd);
}


int handle_command(ftp_session* sess, const string& cmd)
{

    istringstream iss(cmd);
    string cut;
    vector<string> cmds;

    while(iss>>cut){
        cmds.push_back(cut);
    }

    if(cmds.empty()){
        return 0;
    }


    if(cmds[0]=="PASV"){
        handle_pasv(sess);
    }else if(cmds[0]=="LIST"){
        handle_list(sess);
    }else if(cmds[0]=="RETR"){
        if(cmds.size()<2){
            send_response(sess->ctrl_fd,"501 Missing filename");
            return 0;
        }
        handle_retr(sess,cmds[1]);
    }else if(cmds[0]=="STOR"){
        if(cmds.size()<2){
            send_response(sess->ctrl_fd,"501 Missing filename");
            return 0;
        }
        handle_stor(sess,cmds[1]);
    }else if(cmds[0]=="REST"){
        if(cmds.size()<2){
            send_response(sess->ctrl_fd,"501 Missing offset");
            return 0;
        }
        handle_rest(sess,cmds[1]);
    }else if(cmds[0]=="SIZE"){
        if(cmds.size()<2){
            send_response(sess->ctrl_fd,"501 Missing filename");
            return 0;
        }
        handle_size(sess,cmds[1]);
    }else if(cmds[0]=="USER"){
        if(cmds.size()<2){
            send_response(sess->ctrl_fd,"501 Missing username");
            return 0;
        }
        handle_user(sess,cmds[1]);
    }else if(cmds[0]=="PASS"){
        if(cmds.size()<2){
            send_response(sess->ctrl_fd,"501 Missing password");    
            return 0;
        }
        handle_pass(sess,cmds[1]);
    }else if(cmds[0]=="QUIT"){
        handle_quit(sess);
        return 1;
    }else{
        send_response(sess->ctrl_fd,"500 Unknown command");
    }
    return 0;
    


    
}

void handle_user(ftp_session* sess, const string& arg)
{
    if(arg!=CLIENT_NAME){
        send_response(sess->ctrl_fd,"530 Wrong username\r\n");
        return;
    }else{
        send_response(sess->ctrl_fd,"331 Username correct,password required\r\n"); 
        sess->is_right_user=true;      
        return;   
    }


}

void handle_pass(ftp_session* sess, const string& arg){
    if(sess->is_right_user==false){
        send_response(sess->ctrl_fd,"530 No username\r\n");
        return;      
    }
    if(arg!=CLIENT_PASS){
        send_response(sess->ctrl_fd,"530 Wrong password\r\n");   
        return;
    }
    send_response(sess->ctrl_fd,"230 Login successful\r\n");   
    sess->is_login=true;
}

void handle_quit(ftp_session* sess){

    send_response(sess->ctrl_fd,"221 Bye");
    close(sess->data_fd);
    close(sess->data_listen_fd);

}


void handle_pasv(ftp_session* sess)
{
    int listen_fd=socket(AF_INET,SOCK_STREAM,0);
    if(listen_fd<0){
        send_response(sess->ctrl_fd,"425 Cannot create data socket");
        return;
    }

    int opt=1;
    setsockopt(listen_fd,SOL_SOCKET,SO_REUSEADDR,&opt,sizeof(opt));

    sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));

    addr.sin_family=AF_INET;
    addr.sin_addr.s_addr=INADDR_ANY;
    addr.sin_port=0;//系统分配端口

    if(bind(listen_fd,(sockaddr*)&addr,sizeof(addr))<0){
        send_response(sess->ctrl_fd,"425 Failed to bind");
        close(listen_fd);
        return;
    }

    if(listen(listen_fd,1)<0){
        send_response(sess->ctrl_fd,"425 Failed to listen");
        close(listen_fd);
        return;
    }

    socklen_t len=sizeof(addr);
    getsockname(listen_fd,(sockaddr*)&addr,&len);

    int port=ntohs(addr.sin_port);//主机序转换

    sess->data_listen_fd = listen_fd;
    sess->pasv_port=port;

    int p1=port/256;
    int p2=port%256;

    string ip="127,0,0,1";
    string resp="227 Entering Passive Mode (" +ip + "," +to_string(p1) + "," +to_string(p2) + ")";

    send_response(sess->ctrl_fd, resp);
}


void handle_list(ftp_session* sess){

    if(!sess->is_login){
        send_response(sess->ctrl_fd,"530 login first");
        return;
    }
    if(sess->data_listen_fd<0){
        send_response(sess->ctrl_fd,"425 enter PASV first");
        return;
    }

    send_response(sess->ctrl_fd,"150 Opening ASCII mode data connection for file list");//?

    sess->data_fd=accept(sess->data_listen_fd,nullptr,nullptr);

    if(sess->data_fd<0){

        send_response(sess->ctrl_fd,"425 Data connection failed");

        close(sess->data_listen_fd);
        sess->data_listen_fd=-1;
        return;
    }
    DIR* dir=opendir("test_svr");

    if(dir==nullptr){
        send_response(sess->ctrl_fd,"550 Failed to open directory");
        close(sess->data_fd);
        close(sess->data_listen_fd);
        sess->data_fd=-1;
        sess->data_listen_fd=-1;
        return;
    }

    dirent* entry;
    string list_data;

    //遍历
    while((entry=readdir(dir))!=nullptr){
        list_data+=entry->d_name;
        list_data+="\r\n";
    }

    closedir(dir);

    send(sess->data_fd,list_data.c_str(),list_data.size(),0);

    close(sess->data_fd);
    close(sess->data_listen_fd);

    sess->data_fd=-1;
    sess->data_listen_fd=-1;


    send_response(sess->ctrl_fd,"226 Directory send OK");

}


void handle_retr(ftp_session* sess, const string& file){

    if(!sess->is_login){
        send_response(sess->ctrl_fd,"530 login first");
        return;
    }
    if(sess->data_listen_fd<0){
        send_response(sess->ctrl_fd,"425 enter PASV first");
        return;
    }

    //打开文件
    string path="test_svr/"+file;
    int fd=open(path.c_str(),O_RDONLY);

    if(fd<0){
        send_response(sess->ctrl_fd,"550 Failed to open file");
        close(sess->data_listen_fd);
        sess->data_listen_fd=-1;
        return;
    }

    //断点续传:若设置了REST偏移,从偏移处开始读
    if(sess->rest_offset>0){
        lseek(fd, sess->rest_offset, SEEK_SET);
    }
    sess->rest_offset=0;

    send_response(sess->ctrl_fd,"150 Opening binary mode data connection");//待更改

    sess->data_fd=accept(sess->data_listen_fd,nullptr,nullptr);

    if(sess->data_fd<0){

        send_response(sess->ctrl_fd,"425 Data connection failed");
        close(fd);
        close(sess->data_listen_fd);
        sess->data_listen_fd=-1;
        return;
    }
    char buf[BUF_SIZE];
    int n;

    while((n=read(fd,buf,sizeof(buf)))>0){

        send(sess->data_fd,buf,n,0);
    }

    close(fd);

    //关闭数据连接
    close(sess->data_fd);
    close(sess->data_listen_fd);

    sess->data_fd=-1;
    sess->data_listen_fd=-1;

    send_response(sess->ctrl_fd,"226 Transfer complete");
}


void handle_stor(ftp_session* sess, const string& file){

    if(!sess->is_login){
        send_response(sess->ctrl_fd,"530 login first");
        return;
    }

    if(sess->data_listen_fd < 0){
        send_response(sess->ctrl_fd,"425 enter PASV first");
        return;
    }

    string path="test_svr/svr_stor_"+file;

    int flags=O_WRONLY | O_CREAT;
    //断点续传:设置了REST偏移时不截断已有数据,从偏移处开始写
    if(sess->rest_offset==0){
        flags |= O_TRUNC;
    }

    int fd=open(path.c_str(), flags, 0666);

    if(fd<0){
        send_response(sess->ctrl_fd,"550 Failed to create file");
        close(sess->data_listen_fd);
        sess->data_listen_fd = -1;
        return;
    }

    //断点续传:跳到偏移处
    if(sess->rest_offset>0){
        lseek(fd, sess->rest_offset, SEEK_SET);
    }
    sess->rest_offset=0;

    send_response(sess->ctrl_fd,"150 Opening binary mode data connection");

    sess->data_fd=accept(sess->data_listen_fd,nullptr,nullptr);

    if(sess->data_fd<0){

        send_response(sess->ctrl_fd,"425 Data connection failed");
        close(fd);
        close(sess->data_listen_fd);
        sess->data_listen_fd = -1;
        return;
    }

    char buf[BUF_SIZE];
    int n;

    while((n=recv(sess->data_fd,buf,sizeof(buf),0))>0){
        write(fd, buf, n);
    }

    close(fd);
    close(sess->data_fd);
    close(sess->data_listen_fd);

    sess->data_fd=-1;
    sess->data_listen_fd=-1;

    send_response(sess->ctrl_fd,"226 Transfer complete");

}

//断点续传：设置REST偏移,在接下来的RETR/STOR中从该偏移继续
void handle_rest(ftp_session* sess, const string& arg){

    char* end=nullptr;
    long offset=strtol(arg.c_str(), &end, 10);

    if(end==arg.c_str() || *end!='\0'){
        send_response(sess->ctrl_fd,"501 Syntax error in parameters");
        return;
    }

    sess->rest_offset=offset;
    send_response(sess->ctrl_fd,"350 Restarting at " + to_string(offset) + ". Send STORE or RETRIEVE to initiate transfer");
}

//查询服务器已存文件大小,供上传断点续传使用
void handle_size(ftp_session* sess, const string& file){

    if(!sess->is_login){
        send_response(sess->ctrl_fd,"530 login first");
        return;
    }

    string path="test_svr/svr_stor_"+file;//与handle_stor存放路径保持一致

    int fd=open(path.c_str(),O_RDONLY);

    if(fd<0){
        send_response(sess->ctrl_fd,"550 File not found");
        return;
    }

    off_t size=lseek(fd,0,SEEK_END);
    close(fd);

    send_response(sess->ctrl_fd,"213 " + to_string(size));
}


int create_server_socket(int port){

    sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));

    server_addr.sin_family=AF_INET;
    server_addr.sin_port=htons(SERVER_PORT);
    server_addr.sin_addr.s_addr=INADDR_ANY;

    int server_fd=socket(AF_INET,SOCK_STREAM,0);//IPV4,流式
    if(server_fd==-1){
        cout<<"[SERVER] failed to create socket"<<endl;
        return -1;
    }

    int opt=1;
    if(setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0){
        perror("setsockopt failed");
        close(server_fd);
        return -1;
    }

    if(bind(server_fd,(sockaddr*)&server_addr,sizeof(server_addr))){
        cout<<"[SERVER] failed to bind"<<endl;
        close(server_fd);
        return -1;
    }
    if(listen(server_fd,SOMAXCONN)<0){
        cout<<"[SERVER] listen failed"<<endl;
        close(server_fd);
        return -1;
    }



    return server_fd;
}

void send_response(int fd, const string& msg){
    string real_msg=msg;

    if(real_msg.size()<2||real_msg.substr(real_msg.size()-2)!="\r\n"){
        real_msg+="\r\n";
    }

    send(fd,real_msg.c_str(),real_msg.size(),0);

    cout << "[SERVER] " << real_msg;
}