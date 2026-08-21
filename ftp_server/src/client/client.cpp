#include "client.h"


int main(){
    signal(SIGPIPE,SIG_IGN);//忽略SIGPIPE,上传中断时给出错误而不是直接退出
    ftp_client cli;

    cli.server_ip=SERVER_IP;
    cli.server_port=SERVER_PORT;
    cli.is_login=false;
    cli.data_fd=-1;
    cli.rest_offset=0;

    if(connect_server(&cli)<0){
        return -1;
    }
    cout<<recv_resp(&cli);
    while(1){

        string cmd;
        cout<<"ftp> ";

        getline(cin, cmd);

        if(cmd.empty()){
            continue;
        }

        if(handle_command(&cli,cmd)){
            break;
        }
    }
    close(cli.ctrl_fd);
    if(cli.data_fd>0){
        close(cli.data_fd);
    }
    return 0;
}

int connect_server(ftp_client* cli){

    cli->ctrl_fd=socket(AF_INET, SOCK_STREAM, 0);
    if(cli->ctrl_fd<0){
        cout<<"[CLIENT] failed to create socket"<<endl;
        return -1;
    }

    sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));

    server_addr.sin_family=AF_INET;
    server_addr.sin_port=htons(cli->server_port);

    if(inet_pton(AF_INET,cli->server_ip.c_str(),&server_addr.sin_addr)<=0){
        cout<<"[CLIENT] invalid server ip"<<endl;
        close(cli->ctrl_fd);
        return -1;
    }

    if(connect(cli->ctrl_fd,(sockaddr*)&server_addr,sizeof(server_addr))<0){
        cout<<"[CLIENT] failed to connect server"<<endl;
        close(cli->ctrl_fd);
        return -1;
    }

    cout<<"[CLIENT] connected to server"<<endl;

    return 0;
}

void send_cmd(ftp_client* cli, const string& cmd)
{

    string real_cmd=cmd;

    if(real_cmd.size()<2||real_cmd.substr(real_cmd.size()-2)!="\r\n"){
        real_cmd+="\r\n";
    }

    send(cli->ctrl_fd,real_cmd.c_str(),real_cmd.size(),0);

    cout << "[CLIENT] " << real_cmd;
}
string recv_resp(ftp_client* cli){

    char buf[BUF_SIZE];
    memset(buf, 0, sizeof(buf));

    int n=recv(cli->ctrl_fd,buf,sizeof(buf),0);

    if(n<=0){
        return "[CLIENT] server disconnected\r\n";
    }
    
    //转成string返回
    return string(buf);
}
int handle_command(ftp_client* cli, const string& cmd)
{
    istringstream iss(cmd);
    string cut;
    vector<string> cmds;

    while(iss >> cut){
        cmds.push_back(cut);
    }
    if(cmds.empty()){
        return 0;
    }

    if(cmds[0]=="USER"){

        if(cmds.size()<2){
            cout<<"[CLIENT] missing username"<<endl;
            return 0;
        }
        send_cmd(cli, cmd);
        cout<<recv_resp(cli);

    }else if(cmds[0]=="PASS"){

        if(cmds.size()<2){
            cout<<"[CLIENT] missing password" << endl;
            return 0;
        }

        send_cmd(cli, cmd);
        string resp=recv_resp(cli);

        cout<<resp;
        if(resp.substr(0,3)=="230"){
            cli->is_login=true;
        }
    }else if(cmds[0]=="PASV"){

        handle_pasv(cli);

    }else if(cmds[0]=="LIST"){

        handle_list(cli);

    }else if(cmds[0]=="RETR"){

        if(cmds.size()<2){
            cout<<"[CLIENT] missing filename"<<endl;
            return 0;
        }
        handle_retr(cli, cmds[1]);

    }else if(cmds[0]=="STOR"){

        if(cmds.size()<2){
            cout<<"[CLIENT] missing filename"<<endl;
            return 0;
        }
        handle_stor(cli, cmds[1]);
    }else if(cmds[0]=="REST"){

        if(cmds.size()<2){
            cout<<"[CLIENT] missing offset"<<endl;
            return 0;
        }
        handle_rest(cli, cmds[1]);
    }else if(cmds[0]=="QUIT"){

        send_cmd(cli, "QUIT");
        cout<<recv_resp(cli);
        cout<<"[CLIENT] quiting.."<<endl;
        return 1;
    }else{
        send_cmd(cli, cmd);
        cout<<recv_resp(cli);
    }

    return 0;
}

void handle_pasv(ftp_client* cli){

    int data_port;
    string resp;

    string cmd="PASV";
    send_cmd(cli, cmd);
    resp=recv_resp(cli);
    cout<<resp;

//------------------------------解析
    //查找括号
    int left=resp.find('(');
    int right=resp.find(')');

    if(left==string::npos||right==string::npos){
        cout<<"[CLIENT] invalid PASV response"<<endl;
        return;
    }

    string data=resp.substr(left+1,right-left-1);

    stringstream ss(data);
    string cut;
    vector<int> nums;

    while(getline(ss,cut,',')){
        nums.push_back(stoi(cut));
    }

    if(nums.size()!=6){
        cout<<"[CLIENT] invalid PASV data"<<endl;
        return;
    }
    data_port=nums[4]*256+nums[5];
//----------------------------------


    cli->data_fd=socket(AF_INET, SOCK_STREAM, 0);
    if(cli->data_fd<0){
        cout<<"[CLIENT] failed to create socket"<<endl;
        return;
    }
    sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));

    server_addr.sin_family=AF_INET;
    server_addr.sin_port=htons(data_port);

    if(inet_pton(AF_INET,cli->server_ip.c_str(),&server_addr.sin_addr)<=0){
        cout<<"[CLIENT] invalid server ip"<<endl;
        close(cli->data_fd);
        return;
    }

    if(connect(cli->data_fd,(sockaddr*)&server_addr,sizeof(server_addr))<0){
        cout<<"[CLIENT] failed to connect server"<<endl;
        close(cli->data_fd);
        return;
    }

    cout<<"[CLIENT] server entered PASV "<<endl;

    return;

}
void handle_list(ftp_client* cli){

    string cmd="LIST";
    send_cmd(cli,cmd);

    string resp=recv_resp(cli);
    cout<<resp;

    if(resp.substr(0,3)!="150"){
        close(cli->data_fd);
        cli->data_fd=-1;
        return;
    }

    char buf[BUF_SIZE];
    memset(buf,0,sizeof(buf));

    int n;

    cout<<"FILE LIST"<<endl;
    cout<<"--------------------"<<endl;

    while((n=recv(cli->data_fd,buf,sizeof(buf)-1,0))>0){
        buf[n]='\0';
        cout<<buf;
        memset(buf,0,sizeof(buf));
    }

    cout<<"--------------------"<<endl;

    close(cli->data_fd);
    cli->data_fd=-1;


    cout<<recv_resp(cli);


    
}
void handle_retr(ftp_client* cli, const std::string& file){
    string path="test_cli/cli_retr_"+file;

    //断点续传:本地已有部分下载文件时,自动从该大小处继续
    if(cli->rest_offset==0){
        int f=open(path.c_str(),O_RDONLY);
        if(f>=0){
            off_t sz=lseek(f,0,SEEK_END);
            close(f);
            cli->rest_offset=sz;
        }
    }

    //先发送REST设置偏移,服务器从该偏移开始发送
    if(cli->rest_offset>0){
        send_cmd(cli,"REST "+to_string(cli->rest_offset));
        string r=recv_resp(cli);
        cout<<r;
        if(r.substr(0,3)!="350"){
            cli->rest_offset=0;
            return;
        }
    }

    string cmd="RETR "+file;
    send_cmd(cli,cmd);

    string resp=recv_resp(cli);
    cout<<resp;

    if(resp.substr(0,3)!="150"){
        close(cli->data_fd);
        cli->data_fd=-1;
        cli->rest_offset=0;
        return;
    }

    int fd;
    if(cli->rest_offset>0){
        //续传:追加到已有文件末尾
        fd=open(path.c_str(),O_WRONLY|O_CREAT|O_APPEND,0666);
    }else{
        fd=open(path.c_str(),O_WRONLY|O_CREAT|O_TRUNC,0666);
    }

    if(fd<0){
        cout<<"[CLIENT] failed to create local file"<<endl;

        close(cli->data_fd);
        cli->data_fd=-1;
        cli->rest_offset=0;
        return;
    }

    //接收文件数据
    char buf[BUF_SIZE];
    int n;

    cout<<"[CLIENT] downloading..."<<endl;

    while((n=recv(cli->data_fd,buf,sizeof(buf),0))>0){
        write(fd,buf,n);
    }

    cout<<"[CLIENT] download complete"<<endl;

    close(fd);

    //关闭数据连接
    close(cli->data_fd);
    cli->data_fd=-1;
    cli->rest_offset=0;

    cout<<recv_resp(cli);
}
void handle_stor(ftp_client* cli, const string& file){

    string path="test_cli/"+file;

    int fd=open(path.c_str(), O_RDONLY);

    if(fd<0){
        cout<<"[CLIENT] local file not found"<<endl;
        return;
    }

    //断点续传:查询服务器已存文件大小,自动从该大小处继续上传
    if(cli->rest_offset==0){
        send_cmd(cli,"SIZE "+file);
        string szresp=recv_resp(cli);
        cout<<szresp;
        if(szresp.substr(0,3)=="213"){
            cli->rest_offset=strtol(szresp.c_str()+4, nullptr, 10);
        }
    }

    //先发送REST设置偏移,服务器从该偏移开始接收
    if(cli->rest_offset>0){
        send_cmd(cli,"REST "+to_string(cli->rest_offset));
        string r=recv_resp(cli);
        cout<<r;
        if(r.substr(0,3)!="350"){
            close(fd);
            cli->rest_offset=0;
            return;
        }
        //跳过本地已上传部分
        lseek(fd, cli->rest_offset, SEEK_SET);
    }

    string cmd="STOR "+file;
    send_cmd(cli,cmd);

    string resp=recv_resp(cli);
    cout<<resp;

    if(resp.substr(0,3)!="150"){
        close(fd);
        close(cli->data_fd);
        cli->data_fd=-1;
        cli->rest_offset=0;

        return;
    }

    char buf[BUF_SIZE];
    int n;

    cout<<"[CLIENT] uploading..."<<endl;

    while((n=read(fd,buf,sizeof(buf)))>0){
        send(cli->data_fd, buf, n, 0);
    }

    close(fd);
    close(cli->data_fd);
    cli->data_fd=-1;
    cli->rest_offset=0;

    cout<<"[CLIENT] upload complete"<<endl;

    cout<<recv_resp(cli);
}

//手动设置断点续传偏移量,后续RETR/STOR从该偏移继续
void handle_rest(ftp_client* cli, const string& arg){

    char* end=nullptr;
    long offset=strtol(arg.c_str(), &end, 10);

    if(end==arg.c_str() || *end!='\0'){
        cout<<"[CLIENT] invalid offset"<<endl;
        return;
    }

    send_cmd(cli,"REST "+to_string(offset));
    string resp=recv_resp(cli);
    cout<<resp;

    if(resp.substr(0,3)=="350"){
        cli->rest_offset=offset;
    }
}

