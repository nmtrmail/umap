#include "UmapServiceManager.hpp"
#include "umap/util/Macros.hpp"
#include "umap.h"
#include <iostream>
#include <linux/userfaultfd.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <pthread.h>
#include <errno.h>
#include <unistd.h>
#include <stdlib.h>
#include <fcntl.h>
#include <signal.h>
#include <poll.h>
#include <sys/stat.h>

#ifndef SYS_memfd_create
#define SYS_memfd_create 319
#endif
int memfd_create(const char *name, unsigned int flags) {
  return syscall(SYS_memfd_create, name, flags);
}

long init_client_uffd() {
  struct uffdio_api uffdio_api;
  long uffd;

  /* Create and enable userfaultfd object */
  uffd = syscall(__NR_userfaultfd, O_CLOEXEC | O_NONBLOCK);
  if (uffd == -1)
    perror("userfaultfd");

  uffdio_api.api = UFFD_API;
  uffdio_api.features = 0;
  if (ioctl(uffd, UFFDIO_API, &uffdio_api) == -1)
    perror("ioctl-UFFDIO_API");

  return uffd;
}

int setup_uds_connection(int *fd, const char *sock_path){
  struct sockaddr_un sock_addr;
  
  *fd = socket(AF_UNIX, SOCK_STREAM, 0);
  memset(&sock_addr, 0, sizeof(sock_addr));
  sock_addr.sun_family = AF_UNIX;
  strncpy(sock_addr.sun_path, sock_path, sizeof(sock_addr.sun_path));
  if (connect(*fd, (struct sockaddr *) &sock_addr, sizeof(sock_addr)) == -1) {
    close(*fd);
    perror("connect");
    return -1;
  }
  return 0;
}

namespace Umap{
static uint64_t next_region_start_addr = 0x600000000000;

int UmapServInfo::setup_remote_umap_handle(){
  int status = 0;
  ActionParam params;
  params.act = uffd_actions::umap;
  strcpy(params.name, filename.c_str());

  ::write(umap_server_fd, &params, sizeof(params));
  // recieve memfd and region size
  sock_fd_read(umap_server_fd, &(loc), sizeof(region_loc), &(memfd));
  std::cout<<"c: recv memfd ="<<memfd<<" sz ="<<loc.size<<std::endl;

  void* base_addr = mmap(loc.base_addr, loc.size, PROT_READ, MAP_SHARED|MAP_FIXED, memfd, 0);
  if ((int64_t)base_addr == -1) {
    perror("setup_uffd: map failed");
    exit(1);
  }

  std::cout<<"mmap:"<<std::hex<< base_addr<<std::endl;
  //Tell server that the mmap is complete
  ::write(umap_server_fd, "\x00", 1);

  //Wait for the server to register the region to uffd
  sock_recv(umap_server_fd, (char*)&status, 1);
  std::cout<<"Done setting the uffd"<<std::endl;
  return 0;
}

void UmapServInfo::remove_remote_umap_handle()
{
  int status = 0;
  ActionParam params;
  params.act = uffd_actions::unmap;
  strcpy(params.name, filename.c_str());
  ::write(umap_server_fd, &params, sizeof(params));
  sock_recv(umap_server_fd, (char*)&status, 1);
  std::cout<<"Done removing the uffd handling"<<std::endl;
  munmap(loc.base_addr, loc.size);
}

UmapServInfo* ClientManager::cs_umap(std::string filename){
  int umap_server_fd;
  int uffd;
  int fake_var;
  UmapServInfo *ret = NULL;
  if(file_conn_map.find(filename)!=file_conn_map.end()){ 
    UMAP_LOG(Error, "file already mapped for the application");
  }else{
    if(!umap_server_fd){
      if(setup_uds_connection(&umap_server_fd, umap_server_path.c_str())<=0){
        UMAP_LOG(Error, "unable to setup connection with file server");
        return ret;
      }
      uffd = init_client_uffd();
      sock_fd_write(umap_server_fd, &fake_var, sizeof(int), uffd);
      ::close(uffd);
    }
    ret = new UmapServInfo(umap_server_fd, filename, uffd);
    file_conn_map[filename] = ret;
  }
  return ret;
}
      
void ClientManager::cs_uunmap(std::string filename){
  auto it = file_conn_map.find(filename);
  if(it == file_conn_map.end()){
    UMAP_LOG(Error,"No file mapped with the filename");
  }else{
    UmapServInfo* elem = it->second;
    file_conn_map.erase(it);
    delete elem;
  }
}

void* ClientManager::map_req(std::string filename, int prot, int flags){
  auto info = cs_umap(filename);
  if(info){
    return info->loc.base_addr;
  }else
    return NULL;
}

int ClientManager::unmap_req(char *filename){
  auto it = file_conn_map.find(filename);
  if(it==file_conn_map.end()){
    UMAP_LOG(Debug, "unable to find connection with file server");
    return -1;
  }else{
    //TODO: Has to submit the unmap request to the server
    cs_uunmap(filename);
  }
}

int UmapServiceThread::start_thread(){
  if (pthread_create(&t, NULL, ThreadEntryFunc, this) != 0){
    UMAP_ERROR("Failed to launch thread");
    return -1;
  }
  else
    return 0;
}

void *UmapServiceThread::submitUmapRequest(std::string filename, uint64_t csfd){
  struct stat st;
  int memfd=-1;
  int ffd = -1;
  char status;

  mappedRegionInfo *map_reg = mgr->find_mapped_region(filename);
  if(!map_reg){
    ffd = open(filename.c_str(),O_RDONLY);
    if(ffd < 0){
      std::ostringstream errStream;
      errStream << "Error"<<__func__<<"("<<__FILE__<<":"<<__LINE__<<")"<<": Could not open file"<<filename;
      perror(errStream.str().c_str());
      exit(-1);
    }

    fstat(ffd, &st);
    memfd = memfd_create("uffd", 0);
    ftruncate(memfd, st.st_size);
    map_reg = new mappedRegionInfo(ffd, memfd, (void *)next_region_start_addr, st.st_size);
    mgr->add_mapped_region(filename, map_reg);
          //Todo: add error handling code
    next_region_start_addr += st.st_size;
  }
  //Sending the memfd
  sock_fd_write(csfd, (char*)&(map_reg->reg), sizeof(region_loc), map_reg->memfd);
  //Wait for the memfd to get mapped by the client
  sock_recv(csfd, (char*)&status, 1);
  //uffd is already present with the UmapServiceThread
  printf("s: addr: %p uffd: %d map_len=%d\n",map_reg->reg.base_addr,uffd, map_reg->reg.size);
  return Umap::umap_ex(map_reg->reg.base_addr, map_reg->reg.size, /*int prot*/0,/* int flags*/0, ffd, 0, NULL, true, uffd); //prot and flags need to be set 
}

int UmapServiceThread::submitUnmapRequest(std::string filename, uint64_t csfd){
  mappedRegionInfo *map_reg = mgr->find_mapped_region(filename);
  if(map_reg){
    //We could move the ref count of regions at this level
    return uunmap_server(map_reg->reg.base_addr, map_reg->reg.size, uffd); 
  }else{
    UMAP_LOG(Error, "No such file mapped");
    return -1;
  }
}
  
void* UmapServiceThread::serverLoop(){
  ActionParam params;
  int nready;
  struct pollfd pollfds[2]={{ .fd = csfd, .events = POLLIN, .revents = 0 },
           { .fd = pipefds[0], .events = POLLIN | POLLRDHUP | POLLPRI, .revents = 0 }};
  for(;;){
    //Do poll to determine if the client has died
    nready = poll(pollfds, 2, -1);
    if(nready==-1 || pollfds[1].revents){
      break;
    }
    //get the filename and the action from the client
    ::read(csfd, &params, sizeof(params));
    //decode if it is a request to unmap or map
    if(params.act == uffd_actions::umap){
      std::string filename(params.name);
      submitUmapRequest(filename, csfd);
    }else{
      std::string filename(params.name);
      submitUnmapRequest(filename, csfd);
      //yet to implement submitUnmapRequest
    }
    //operation completed
    ::write(csfd, "\x00", 1);
    pollfds[0].revents = 0;
    pollfds[1].revents = 0;
  }
}

void UmapServerManager::start_service_thread(int csfd, int uffd){
  UmapServiceThread *t = new UmapServiceThread(csfd, uffd, this);
  if(!t->start_thread())
    service_threads.push_back(t);
}

void UmapServerManager::stop_service_threads(){
  UmapServiceThread *t;
  while(!service_threads.empty()){
    t = service_threads.back();
    service_threads.pop_back();
    delete t;
  }
}

void UmapServerManager::add_mapped_region(std::string filename, mappedRegionInfo* m){
  file_to_region_map[filename] = m;
}

void start_umap_service(int csfd){
  int fake_var;
  int uffd;
  UmapServerManager *usm = UmapServerManager::getInstance();
  sock_fd_read(csfd, &fake_var, sizeof(int), &uffd);
  usm->start_service_thread(csfd, uffd);
}

} //End of Umap namespace

void umap_server(std::string filename){
  int sfd = socket(AF_UNIX, SOCK_STREAM, 0);
  struct sockaddr_un addr;

  memset(&addr, 0, sizeof(addr));
  snprintf(addr.sun_path, sizeof(addr.sun_path), "/tmp/uffd-server");
  addr.sun_family = AF_UNIX;
  unlink(addr.sun_path);
  bind(sfd, (struct sockaddr*)&addr, sizeof(addr));
        
  listen(sfd, 256);
  for (;;) {
    int cs = accept(sfd, 0, 0);
    if (cs == -1) {
      perror("accept");
      exit(1);
    }
    Umap::start_umap_service(cs);
  }
  close(sfd);
  unlink(addr.sun_path);
}