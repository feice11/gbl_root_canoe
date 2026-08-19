#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

#define REGION (1024*1024-3*1024)
#define SLOT (REGION/2)
#pragma pack(push,1)
struct hdr { char magic[4]; uint16_t version,header_bytes,width,height; uint8_t scale,format; uint16_t reserved; uint32_t data_bytes,crc32,sequence; };
#pragma pack(pop)
static uint32_t crc32(const uint8_t *p,size_t n){uint32_t c=~0u;while(n--){int i;c^=*p++;for(i=0;i<8;i++)c=(c>>1)^((0-(c&1))&0xedb88320u);}return ~c;}
static void le16(uint8_t *p,uint16_t v){p[0]=v;p[1]=v>>8;}static void le32(uint8_t*p,uint32_t v){p[0]=v;p[1]=v>>8;p[2]=v>>16;p[3]=v>>24;}
int main(int argc,char **argv){
  int fd=-1,out=-1,rc=1,selected=-1;off_t end,off;uint8_t *buf=NULL,*rgb=NULL,*row=NULL;struct hdr *h=NULL;size_t pos,pixels=0,total,rowbytes,y;
  if(argc!=3)return 2;
  fd=open(argv[1],O_RDWR|O_CLOEXEC);if(fd<0)goto done;
  end=lseek(fd,0,SEEK_END);if(end<(off_t)(1024*1024))goto done;
  off=end-1024*1024;
  buf=malloc(REGION);if(!buf||pread(fd,buf,REGION,off)!=(ssize_t)REGION)goto done;
  {int i;for(i=0;i<2;i++){struct hdr *candidate=(struct hdr*)(buf+i*SLOT);uint8_t *payload=(uint8_t*)candidate+sizeof(*candidate);
    if(!memcmp(candidate->magic,"SFSS",4)&&candidate->version==1&&candidate->header_bytes==sizeof(*candidate)&&candidate->width&&candidate->height&&candidate->format==1&&candidate->data_bytes<=SLOT-sizeof(*candidate)&&crc32(payload,candidate->data_bytes)==candidate->crc32){
      if(selected<0||candidate->sequence>h->sequence){selected=i;h=candidate;}
    }}if(selected<0)goto done;}
  total=(size_t)h->width*h->height;rgb=malloc(total*2);if(!rgb)goto done;
  {uint8_t *payload=(uint8_t*)h+sizeof(*h);pos=0;while(pos<h->data_bytes&&pixels<total){uint8_t tag=payload[pos++];size_t count=(tag&127)+1,i;if(tag&128){uint16_t v;if(pos+2>h->data_bytes)goto done;v=payload[pos]|((uint16_t)payload[pos+1]<<8);pos+=2;for(i=0;i<count&&pixels<total;i++){rgb[pixels*2]=v;rgb[pixels*2+1]=v>>8;pixels++;}}else{if(pos+count*2>h->data_bytes)goto done;memcpy(rgb+pixels*2,payload+pos,count*2);pos+=count*2;pixels+=count;}}}
  if(pixels!=total)goto done;
  out=open(argv[2],O_CREAT|O_TRUNC|O_WRONLY|O_CLOEXEC,0644);if(out<0)goto done;
  rowbytes=((size_t)h->width*3+3)&~3u;row=calloc(1,rowbytes);if(!row)goto done;
  {uint8_t bh[54]={0};memcpy(bh,"BM",2);le32(bh+2,54+rowbytes*h->height);le32(bh+10,54);le32(bh+14,40);le32(bh+18,h->width);le32(bh+22,h->height);le16(bh+26,1);le16(bh+28,24);le32(bh+34,rowbytes*h->height);if(write(out,bh,54)!=54)goto done;}
  for(y=h->height;y>0;y--){size_t x;memset(row,0,rowbytes);for(x=0;x<h->width;x++){uint16_t v=rgb[((y-1)*h->width+x)*2]|((uint16_t)rgb[((y-1)*h->width+x)*2+1]<<8);row[x*3]=(v&31)*255/31;row[x*3+1]=((v>>5)&63)*255/63;row[x*3+2]=((v>>11)&31)*255/31;}if(write(out,row,rowbytes)!=(ssize_t)rowbytes)goto done;}
  fsync(out);{uint32_t zero=0;if(pwrite(fd,&zero,4,off+(off_t)selected*SLOT)!=4)goto done;fsync(fd);}rc=0;
done:if(out>=0)close(out);if(fd>=0)close(fd);free(row);free(rgb);free(buf);if(rc)unlink(argv[2]);return rc;
}
