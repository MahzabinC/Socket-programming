#include <netinet/in.h> //structure for storing address information
#include <stdio.h>
#include <stdlib.h>
#include <sys/socket.h> //for socket APIs
#include <sys/types.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>  //for inet_pton()
#include <fcntl.h>
#include <sys/stat.h>
#include <endian.h>  //linux
//#include <libkern/OSByteOrder.h> //be64toh htobe64

#define BUFFER_SIZE 4096
#define SERVER_PORT 6666
#define SERVER_IP "127.0.0.1"


// Client file

//Function to check if the extension is valid (.c,.txt,.pdf,.zip)
int valid_extension(const char *filename) {
    const char *ext = strrchr(filename, '.');
    if (!ext) return 0;
    return strcmp(ext, ".c") == 0 || strcmp(ext, ".txt") == 0 || strcmp(ext, ".pdf") == 0 || strcmp(ext, ".zip") == 0;
}
// Functiion to write in socket making sure its uint32
void write_uint32(int socket, uint32_t length) {
    length = htonl(length);
    write(socket, &length, sizeof(length));
}
// Functiion to write in socket making sure its uint64
void write_uint64(int sock, uint64_t val) {
    val = htobe64(val);
    //val = OSSwapHostToBigInt64(val);
    write(sock, &val, sizeof(val));
}
// Functiion to read from socket making sure its uint64
uint64_t read_uint64(int socket) {
    uint64_t val;
    read(socket, &val, sizeof(val));
    return be64toh(val);
    //return OSSwapBigToHostInt64(val);
}
// Functiion to read from socket making sure its uint32
uint32_t read_uint32(int socket) {
    uint32_t val;
    read(socket, &val, sizeof(val));
    return ntohl(val);
}

// Function that handles work for uploading a file
void upload_file(int socket, char *filename, char *destination){
    const int fd = open(filename, O_RDONLY);
    if (fd < 0) {
        perror("File open failed");
        return;
    }
    char meow[BUFFER_SIZE];

    write(socket, "uloadf", 6);
    write_uint32(socket, strlen(filename));            // writing byte size of filename
    write(socket, filename, strlen(filename));         // actual filename
    write_uint32(socket, strlen(destination));         // writing byte size of destination path in socket
    write(socket, destination, strlen(destination));   //actual destination path

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
    printf("Successfully uploaded '%s' to '%s'\n", filename, destination);
}

// Function for downloading a file from server
void download_file(int socket, char *path){

    write(socket, "downlf", 6);
    write_uint32(socket, strlen(path));               // writing byte size of path
    write(socket, path, strlen(path));                // actual path

    // First get file size
    printf("Client: Waiting for file size...\n");
    uint64_t file_size = read_uint64(socket);
    printf("Client: Received file size = %llu\n", file_size);
    fflush(stdout);
    if (file_size == 0) {
        printf("File not found on server.\n");
        return;
    }

    const char *filename = strrchr(path, '/');
    if (!filename) filename = path;
    else filename++;
    printf("filename %s\n", filename);
    fflush(stdout);
    int fd = open(filename, O_CREAT | O_WRONLY | O_TRUNC, 0644);

    char buffer[BUFFER_SIZE];
    while (file_size > 0) {
        // printf("File created");
        // fflush(stdout);
        ssize_t rb = read(socket, buffer, file_size > BUFFER_SIZE ? BUFFER_SIZE : file_size);
        if (rb <= 0) {
            perror("read failed");
            break;
        }
        ssize_t wb = write(fd, buffer, rb);
        // printf("Client: reading and writing in file = %ld\n", wb);
        // fflush(stdout);
        if (wb != rb) {
        perror("write mismatch");
        close(fd);
        return;
        }
        file_size -= rb;
        memset(buffer,0,sizeof(buffer));
    }
    close(fd);
    printf("Downloaded '%s'\n", filename);
}

// Function to send server information about file to be deleted.
void remove_file(int socket, char *path){
  write(socket, "removf", 6);
  write_uint32(socket, strlen(path));               // writing byte size of path
  write(socket, path, strlen(path));                // actual path
  printf("Removing '%s'\n", path);
}

// Function to get name of all files in a path in server
void display_filename(int socket, char *path){
  write(socket, "dispfn", 6);
  write_uint32(socket, strlen(path));               // writing byte size of path
  write(socket, path, strlen(path));                // actual path
  char filename[256] = {0};
  for (int j = 0; j < 4; j++) {
    uint32_t c = read_uint32(socket);
    // printf("count %u\n", c);
    // fflush(stdout);
    for (uint32_t i = 0; i < c; i++) {
        uint32_t name_size = read_uint32(socket);
        read(socket, filename, name_size);
        filename[name_size] = '\0';
        printf("%s\n", filename);
        // printf("lallaallala");
        fflush(stdout);
        memset(filename, 0, sizeof(filename));
    }
  }
}

// Function to get tar of a specified type of file from server
void download_tar(int socket, char *ext){
  write(socket, "dnltar", 6);
  write(socket, ext, strlen(ext));                // actual path
  char buffer[BUFFER_SIZE];
  char tarname[32];
  uint64_t file_size = read_uint64(socket);
  if (file_size == 0) {
        printf("Didn't find any files of type %s.\n", ext);
        return;
  }

  if (strcmp(ext, ".c") == 0){
    strcpy(tarname, "cfiles.tar");
  }
  else if (strcmp(ext, ".pdf") == 0){
    strcpy(tarname, "pdffiles.tar");
  }
  else if (strcmp(ext, ".txt") == 0){
    strcpy(tarname, "textfiles.tar");
  }

  int fd = open(tarname, O_CREAT | O_WRONLY | O_TRUNC, 0644);
  if (fd < 0) {
      perror("Error creating tar file");
      return;
  }
  while (file_size > 0) {
      ssize_t rb = read(socket, buffer, file_size > BUFFER_SIZE ? BUFFER_SIZE : file_size);
      write(fd, buffer, rb);
      file_size -= rb;
      memset(buffer,0,sizeof(buffer));
  }
  close(fd);
  printf("Downloaded tar to '%s'\n", tarname);
}
// Main Function
int main(){
    int client_socket;
    struct sockaddr_in server_address;

    if ((client_socket=socket(AF_INET,SOCK_STREAM,0))<0){ //socket()
        fprintf(stderr, "Cannot create socket\n");
        exit(1);
    }
    //ADD the server's PORT NUMBER AND IP ADDRESS TO THE sockaddr_in object
    server_address.sin_family = AF_INET;
    server_address.sin_port = htons((uint16_t)SERVER_PORT);
    if(inet_pton(AF_INET, SERVER_IP,&server_address.sin_addr) < 0){
        fprintf(stderr, " inet_pton() has failed\n");
        exit(2);
    }

    if(connect(client_socket, (struct sockaddr  *) &server_address,sizeof(server_address))<0){  //Connect()
        fprintf(stderr, "connect() failed, exiting\n");
        exit(3);
    }

    printf("Connected to S1 server on %s:%d\n", SERVER_IP, SERVER_PORT);

    char input[512];
    while (1) {
        printf("w25clients$ ");
        fgets(input, sizeof(input), stdin);
        input[strcspn(input, "\n")] = 0;

        // Parse command
        char command[16], filename[256], destination_path[256];
        int args = sscanf(input, "%s %s %s", command, filename, destination_path);
        if(args>3 || args<2){
           printf("Usage: command filename destination_path\n");
           printf("or\n");
           printf("Usage: command file_path\n");
           continue;
        }

        if (strcmp(command, "uploadf") == 0) {
            if (args != 3) {
              printf("Usage: uploadf filename destination_path\n");
              continue;
            }
            if(!valid_extension(filename)){
                printf("Valid filenames include (.c /.pdf/ .txt/.zip\n");
                continue;

            }
            upload_file(client_socket, filename, destination_path);
        }
        else if (strcmp(command, "downlf") == 0) {
            if (args != 2) {
                printf("Usage: downlf filepath\n");
                continue;
            }
            if(!valid_extension(filename)){
                    printf("Valid filenames include (.c /.pdf/ .txt/.zip\n");
                    continue;

            }
            download_file(client_socket, filename);

        }
        else if(strcmp(command,"removef") == 0){
            if (args != 2) {
                printf("Usage: removef filepath\n");
                continue;
            }
            if(!valid_extension(filename)){
                    printf("Valid filenames include (.c /.pdf/ .txt/.zip\n");
                    continue;

            }
            remove_file(client_socket, filename);

        }
        else if(strcmp(command,"downltar") == 0){
            if (args != 2) {
                printf("Usage: downltar file\n");
                continue;
            }
            if (strcmp(filename, ".c") != 0 && strcmp(filename, ".pdf") != 0 && strcmp(filename, ".txt") != 0) {
                printf("Invalid filetype. Only .c, .pdf, .txt are allowed.\n");
                continue;

            }
            download_tar(client_socket, filename);

        }
        else if(strcmp(command,"dispfnames") == 0){
            if (args != 2) {
                printf("Usage: dispfnames pathname\n");
                continue;
            }
            display_filename(client_socket, filename);

        }
        else {
            printf("Unknown or unsupported command.\n");
        }
        memset(input,0,sizeof(input));
    }
}
