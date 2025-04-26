#include <netinet/in.h> //structure for storing address information
#include <stdio.h>
#include <stdlib.h>
#include <sys/socket.h> //for socket APIs
#include <sys/types.h>
#include <string.h>
#include <unistd.h>    // declares fork(), pipe(), read(), write(), etc.
#include <fcntl.h>
#include <sys/stat.h>
#include <errno.h>
#include <arpa/inet.h>  //for inet_pton()
//#include <libkern/OSByteOrder.h> //be64toh htobe64
#include <endian.h>  // linux
#include <dirent.h>
#include <signal.h>
#include <ftw.h>

char collected_files[100][256];
int file_count = 0;

#define PORT 6666
#define S2_PORT 6667
#define S3_PORT 6668
#define S4_PORT 6669
#define BUFFER_SIZE 4096
#define SERVER_IP "127.0.0.1"



//making sure no process becomes zombie
void handle_sigchld(int sig) {
    // Reap all dead children without blocking
    while (waitpid(-1, NULL, WNOHANG) > 0);
}
// Utility: Read and write helpers
uint32_t read_uint32(int socket) {
    uint32_t val;
    read(socket, &val, sizeof(val));
    return ntohl(val);
}
uint64_t read_uint64(int socket) {
    uint64_t val;
    read(socket, &val, sizeof(val));
    return be64toh(val);
    //return OSSwapBigToHostInt64(val);
}
void write_uint32(int socket, uint32_t length) {
    length = htonl(length);
    write(socket, &length, sizeof(length));
}

void write_uint64(int sock, uint64_t val) {
    //val = OSSwapHostToBigInt64(val);
    val = htobe64(val);
    write(sock, &val, sizeof(val));
}
//Create directories recursively
int mkdir_p(const char *path) {
    char temp[512];
    char *p = NULL;
    size_t len;

    // Expand tilde if needed
    if (path[0] == '~') {
        const char *home = getenv("HOME");
        if (!home) return -1;
        snprintf(temp, sizeof(temp), "%s/%s", home, path + 1);
        path = temp;
    }
    strncpy(temp, path, sizeof(temp));
    len = strlen(temp);

    // Remove trailing slash
    if (temp[len - 1] == '/') {
        temp[len - 1] = '\0';
    }

    for (p = temp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(temp, 0755) != 0 && errno != EEXIST) {
                perror("mkdir");
                return -1;
            }
            *p = '/';
        }
    }
    if (mkdir(temp, 0755) != 0 && errno != EEXIST) {
        perror("mkdir");
        return -1;
    }
    return 0;
}
// === Forward upload to S2/S3/S4 depending on extension ===
void forward(int client_socket, char *filename,  uint64_t file_size, char *destination,int server_port, const char *server_ip){
  int ss_socket;
  struct sockaddr_in ss_address;
  if ((ss_socket=socket(AF_INET,SOCK_STREAM,0))<0){
    fprintf(stderr, "Cannot create socket\n");
    exit(1);
  }
  //ADD the server's PORT NUMBER AND IP ADDRESS TO THE sockaddr_in object
  ss_address.sin_family = AF_INET;
  ss_address.sin_port = htons((uint16_t)server_port);
  if(inet_pton(AF_INET, server_ip,&ss_address.sin_addr) < 0){
      perror("inet_pton failed");
      return;
  }
  if(connect(ss_socket, (struct sockaddr  *) &ss_address,sizeof(ss_address))<0){  //Connect()
      perror("Connection to other servers failed");
      return;
  }
  write(ss_socket, "uloadf", 6);
  write_uint32(ss_socket, strlen(filename));            // writing byte size of filename
  write(ss_socket, filename, strlen(filename));         // actual filename
  write_uint32(ss_socket, strlen(destination));         // writing byte size of destination path in socket
  write(ss_socket, destination, strlen(destination));   //actual destination path
  write_uint64(ss_socket, file_size);
  char buffer[BUFFER_SIZE];
  while (file_size > 0) {
    ssize_t rb = read(client_socket, buffer, file_size > BUFFER_SIZE ? BUFFER_SIZE : file_size);
    if (rb <= 0) break;
    write(ss_socket, buffer, rb);
    file_size -= rb;
    memset(buffer,0,sizeof(buffer));
  }
}
// === Handles upload logic based on file extension ===
void uploadf(int socket){
  char destination[256]={0};
  char filename[256]={0};

  uint32_t filename_size = read_uint32(socket);
  read(socket, filename, filename_size);
  uint32_t destination_size = read_uint32(socket);
  read(socket, destination, destination_size);

  const char *ext = strrchr(filename, '.');
  uint64_t file_size = read_uint64(socket);
  printf("extension %s\n",ext);
    fflush(stdout);
  if (strcmp(ext, ".pdf") == 0) {
    // sending file to server 2 if pdf
        forward(socket, filename, file_size, destination, S2_PORT, "127.0.0.1");
  }
  else if (strcmp(ext, ".txt") == 0) {
    //sending file to server 3 if txt
        forward(socket, filename, file_size, destination, S3_PORT, "127.0.0.1");
  }
  else if (strcmp(ext, ".zip") == 0) {
    // sending file to server 4 if zip
        forward(socket, filename, file_size, destination, S4_PORT, "127.0.0.1");
  }
  else {
     char path[512];
      if (destination[0] == '~') {
          const char *home = getenv("HOME");
          if (home) {
              char expanded[512];
              snprintf(expanded, sizeof(expanded), "%s/%s", home, destination + 1);
              strcpy(destination, expanded);
          }
      }
     snprintf(path, sizeof(path), "%s/%s", destination, filename);
     mkdir_p(destination); // ensure dir exists

     const int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
      printf("Resolved path: %s\n", path);
     if(fd<0){
        perror("failed to create file");
     }
     char buffer[BUFFER_SIZE];
     while (file_size > 0) {
       ssize_t rb = read(socket, buffer, file_size > BUFFER_SIZE ? BUFFER_SIZE : file_size);
       if (rb <= 0) break;
       ssize_t wb = write(fd, buffer, rb);
       if (wb != rb) {
        perror("write mismatch");
        close(fd);
        return;
       }
       file_size -= rb;
         memset(buffer,0,sizeof(buffer));
     }

     close(fd);
     printf("S1 Stored %s locally\n", filename);
  }
}


// Function to forward to S2,S3,S4 for commands other than upload
void for_ward(char *command, int client_socket, uint32_t path_size, char *path, int server_port, const char *server_ip){
  int ss_socket;
  struct sockaddr_in ss_address;
  if ((ss_socket=socket(AF_INET,SOCK_STREAM,0))<0){
    fprintf(stderr, "Cannot create socket\n");
    exit(1);
  }
  //ADD the server's PORT NUMBER AND IP ADDRESS TO THE sockaddr_in object
  ss_address.sin_family = AF_INET;
  ss_address.sin_port = htons((uint16_t)server_port);
  if(inet_pton(AF_INET, server_ip,&ss_address.sin_addr) < 0){
      perror("inet_pton failed");
      return;
  }
  if(connect(ss_socket, (struct sockaddr  *) &ss_address,sizeof(ss_address))<0){  //Connect()
      perror("Connection to other servers failed");
      return;
  }
  write(ss_socket, command, 6);
  write_uint32(ss_socket, path_size);                   // writing byte size of destination path in socket
  write(ss_socket, path, path_size);                    //actual destination path

  if(strcmp(command,"downlf")==0){
    char buffer[BUFFER_SIZE];
    uint64_t file_size = read_uint64(ss_socket);
    write_uint64(client_socket, file_size);
    while (file_size > 0) {
        ssize_t rb = read(ss_socket, buffer, file_size > BUFFER_SIZE ? BUFFER_SIZE : file_size);
        if (rb <= 0) break;
        ssize_t wb = write(client_socket, buffer, rb);
        if (wb != rb) {
        perror("write mismatch");
        return;
        }
        file_size -= rb;
        memset(buffer,0,sizeof(buffer));
    }
  }
  else if(strcmp(command, "dispfn") == 0){
    char filename[256];
    uint32_t count = read_uint32(ss_socket);
    write_uint32(client_socket, count);
    printf("[S1] Forwarding %d filenames from server on port %d\n", count, server_port);
    fflush(stdout);

    // For each filename from the other server
    for(int i = 0; i < count; i++){
      uint32_t name_size = read_uint32(ss_socket);
      memset(filename, 0, sizeof(filename));

      // Read the filename from other server
      read(ss_socket, filename, name_size);

      // Forward to client
      write_uint32(client_socket, name_size);
      write(client_socket, filename, name_size);  // Only write the actual data, not the whole buffer

      printf("[S1] Forwarded filename '%s' to client\n", filename);
      fflush(stdout);
    }
  }
  else if(strcmp(command, "dnltar") == 0){
    char buffer[BUFFER_SIZE];
    uint64_t file_size = read_uint64(ss_socket);
    write_uint64(client_socket, file_size);

    while (file_size > 0) {
        ssize_t rb = read(ss_socket, buffer, file_size > BUFFER_SIZE ? BUFFER_SIZE : file_size);
        write(client_socket, buffer, rb);
        printf("S1 TAR reading from ss socket writing in client");
        fflush(stdout);
        file_size -= rb;
        memset(buffer,0,sizeof(buffer));
    }
  }
  close(ss_socket);
}
// Function to handle download operation
void downloadf(int socket){
  char path[256]={0};
  uint32_t path_size = read_uint32(socket);
  read(socket, path, path_size);
  char full_path[512];
  if (path[0] == '~') {
      const char *home = getenv("HOME");
      if (home) {
          snprintf(full_path, sizeof(full_path), "%s/%s", home, path + 1);
      }
  }
  //printf("path for c down %s\n", full_path);
  const char *filename = strrchr(full_path, '/'); // getting filename from the path
  const char *ext = strrchr(filename, '.');  // getting extension from filename
  printf("S1 extension for download %s\n",ext);
    fflush(stdout);

  if (strcmp(ext, ".pdf") == 0) {
    // sending file to server 2 if pdf
        for_ward("downlf", socket, path_size, path, S2_PORT, "127.0.0.1");
  }
  else if (strcmp(ext, ".txt") == 0) {
    //sending file to server 3 if txt
      //printf("made it here\n");
      fflush(stdout);  //forces output to appear
      for_ward("downlf", socket, path_size, path, S3_PORT, "127.0.0.1");
  }
  else if (strcmp(ext, ".zip") == 0) {
    // sending file to server 4 if zip
        for_ward("downlf", socket, path_size, path, S4_PORT, "127.0.0.1");
  }
  else {

     int fd = open(full_path, O_RDONLY);
     if (fd<0) {
         printf("Cant open file S1 download");
         return;
     }
     char meow[BUFFER_SIZE];
     // getting file size and sending it in socket
     struct stat st;
     if (fstat(fd, &st) != 0) {
        perror("fstat failed");
        close(fd);
        return;
     }
     uint64_t file_size = st.st_size;
     write_uint64(socket, file_size);

     ssize_t bytes;
     while ((bytes = read(fd, meow, sizeof(meow))) > 0) {
        write(socket, meow, bytes);
         memset(meow,0,sizeof(meow));
     }
     close(fd);
  }
}
// Function to handle remove operations
void removef(int socket){
  char path[256]={0};
  uint32_t path_size = read_uint32(socket);
  read(socket, path, path_size);

  const char *filename = strrchr(path, '/'); // getting filename from the path
  const char *ext = strrchr(filename, '.');  // getting extension from filename

  if (strcmp(ext, ".pdf") == 0) {
    // sending file to server 2 if pdf
        for_ward("removf", socket, path_size, path, S2_PORT, "127.0.0.1");
  }
  else if (strcmp(ext, ".txt") == 0) {
    //sending file to server 3 if txt
        for_ward("removf", socket, path_size, path, S3_PORT, "127.0.0.1");
  }
  else if (strcmp(ext, ".zip") == 0) {
    // sending file to server 4 if zip
        for_ward("removf", socket, path_size, path, S4_PORT, "127.0.0.1");
  }
  else {
    char temp[512];
    // Expand tilde if needed
    if (path[0] == '~') {
        const char *home = getenv("HOME");
        if (!home){perror("unable to get home");}
        snprintf(temp, sizeof(temp), "%s/%s", home, path + 1);
    }
    if(remove(temp)==0){
      printf("File %s successfully deleted from Server",filename);
    }

  }
}
// function that compares file names
int compare_filenames(const void *a, const void *b) {
    const char *fa = (const char *)a;
    const char *fb = (const char *)b;
    return strcmp(fa, fb);
}


// Recursive callback for each file
int list_files_cb(const char *fpath, const struct stat *sb, int typeflag, struct FTW *ftwbuf) {
    if (typeflag == FTW_F) {
        const char *filename = strrchr(fpath, '/');
        if (filename) filename++;
        else filename = fpath;

        if (filename[0] != '.') {
            strncpy(collected_files[file_count], filename, sizeof(collected_files[file_count]) - 1);
            file_count++;
        }
    }
    return 0; // Continue
}

void display_fnames(int socket) {
    uint32_t path_size = read_uint32(socket);
    char path[256];
    read(socket, path, path_size);
    path[path_size] = '\0';

    printf("path: %s\n", path);
    fflush(stdout);

    // Expand tilde
    char full_path[512];
    if (path[0] == '~') {
        const char *home = getenv("HOME");
        snprintf(full_path, sizeof(full_path), "%s/%s", home, path + 1);
    } else {
        snprintf(full_path, sizeof(full_path), "%s", path);
    }

    // Reset collection buffer
    file_count = 0;
    memset(collected_files, 0, sizeof(collected_files));

    // Walk directory recursively
    nftw(full_path, list_files_cb, 10, FTW_PHYS);
    // sort before sending
    qsort(collected_files, file_count, sizeof(collected_files[0]), compare_filenames);

    write_uint32(socket, file_count);
    for (int i = 0; i < file_count; i++) {
        write_uint32(socket, strlen(collected_files[i]));
        write(socket, collected_files[i], strlen(collected_files[i]));
        printf("[S1] Sent %s to client\n", collected_files[i]);
    }

    // Forward to other servers
    for_ward("dispfn", socket, path_size, path, S2_PORT, "127.0.0.1");
    for_ward("dispfn", socket, path_size, path, S3_PORT, "127.0.0.1");
    for_ward("dispfn", socket, path_size, path, S4_PORT, "127.0.0.1");
}

// Function to handle tar file download
void download_tarf(int socket){
    char root[256]={0};
    char filetype[5] = {0};
    read(socket, filetype, 4);
    const char *home = getenv("HOME");
    snprintf(root, sizeof(root), "%s/S1", home);
    printf("root: %s\n",root);
    fflush(stdout);

    if (strcmp(filetype, ".c") == 0) {
        char cmd[512];
        snprintf(cmd, sizeof(cmd), "cd %s && find . -type f -name '*.c' | tar -cf cfiles.tar -T -", root);
        printf("cmd: %s\n",cmd);
        fflush(stdout);
        system(cmd);

        char tarpath[512];
        snprintf(tarpath, sizeof(tarpath), "%s/cfiles.tar", root);
        printf("tarpath: %s\n",tarpath);
        fflush(stdout);
        int fd = open(tarpath, O_RDONLY);
        if (fd < 0) {
            write_uint64(socket, 0);
            printf("failed to open tar");
            fflush(stdout);
            return;
        }
        struct stat st;
        fstat(fd, &st);
        write_uint64(socket, st.st_size);

        char buffer[BUFFER_SIZE];
        ssize_t r;
        while ((r = read(fd, buffer, sizeof(buffer))) > 0) {
            write(socket, buffer, r);
            memset(buffer,0,sizeof(buffer));
        }
        close(fd);
        unlink(tarpath);
    } else if (strcmp(filetype, ".pdf") == 0) {
        for_ward("dnltar", socket, sizeof(filetype), filetype, S2_PORT, "127.0.0.1");
    } else if (strcmp(filetype, ".txt") == 0) {
        for_ward("dnltar", socket, sizeof(filetype), filetype, S3_PORT, "127.0.0.1");
    } else {
        write_uint64(socket, 0); // Not supported
    }
}

void prcclient(int connection_socket){
  char cmd[7] = {0};

    while (recv(connection_socket, cmd, 6, MSG_WAITALL) == 6) {
        printf("[S1] Received command: %s\n", cmd);

        if (strcmp(cmd, "uloadf") == 0) {
            uploadf(connection_socket);
        } else if (strcmp(cmd, "downlf") == 0) {
            downloadf(connection_socket);
        } else if (strcmp(cmd, "removf") == 0) {
            removef(connection_socket);
        } else if (strcmp(cmd, "dnltar") == 0) {
            download_tarf(connection_socket);
        } else if (strcmp(cmd, "dispfn") == 0) {
            display_fnames(connection_socket);
        } else {
            printf("Invalid command: %s\n", cmd);
        }
        memset(cmd, 0, sizeof(cmd));  // Clear buffer for next command
    }

    close(connection_socket);
    printf("[S1] Client disconnected.\n");
}

int main(){// Server 1 (Main Server) stores .c
    signal(SIGCHLD, handle_sigchld);
    int listening_socket, connection_socket;
    socklen_t len;
    struct sockaddr_in server_address, client_address; //Socket address object to which IP address and port number are added

    //socket() sytem call
    if((listening_socket = socket(AF_INET, SOCK_STREAM, 0))<0){
        fprintf(stderr, "Could not create socket\n");
        exit(1);
    }
    //Add port number and IP address to servAdd before invoking the bind() system call
    server_address.sin_family = AF_INET;
    inet_pton(AF_INET, SERVER_IP, &server_address.sin_addr); //Add the IP address of the machine
    server_address.sin_port = htons((uint16_t)PORT);//Add the port number entered by the user
    //bind() system call
    bind(listening_socket, (struct sockaddr *) &server_address,sizeof(server_address));

    //listen
    listen(listening_socket, 10);
    printf("[S1] Listening on port %d...\n", PORT);

    while (1){
      connection_socket= accept(listening_socket,(struct sockaddr *)&client_address,&len); //accept()
      if(connection_socket==-1){
        perror("Request Accept Failed");
      }
      int pid= fork();
      if(pid<0){
       perror("Fork Failed");
      }
      else if(pid==0){
        // Child process
        close(listening_socket); // child doesn't need this
        prcclient(connection_socket);
        //close(connection_socket);
        exit(0);
      }
      else{
        close(connection_socket); // Parent process
      }
    }
}
