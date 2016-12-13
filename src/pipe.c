/*
 * Copyright (C) 2014 Espen Jürgensen <espenjurgensen@gmail.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#ifdef HAVE_CONFIG_H
# include <config.h>
#endif

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdint.h>
#include <fcntl.h>
#include <string.h>
#include <time.h>
#include <errno.h>
#include <worker.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <event2/event.h>

#include <pthread.h>

#include "pipe.h"
#include "logger.h"
#include "commands.h"
#include "conffile.h"
#include "logger.h"
#include "misc.h"

#define PIPE_BUFFER_SIZE 8192
#define META_PIPE_BUFFER_SIZE 65535
#define META_PIPE_BUFFER_MAXSIZE 524288
#define TIMEOUT_PIPEREAD_SEC 1



static int g_fd = -1;
static uint16_t *g_buf = NULL;

struct metadata_buf {
  char* buf;
  int pos;
  int size;
}; 

/* --- Globals --- */
// cache thread
static pthread_t tid_pipewatcher;
static int g_initialized;

// Event base, pipes and events
struct event_base *g_evbase_pipewatcher;
static struct event *g_pipewatcher_audioev;
static struct event *g_pipewatcher_metadataev;
static struct commands_base *cmdbase;
static struct metadata_buf g_metabuf;

//temp variables for pipe watching ( need to be revised)
static int g_fpipe = -1;
static int g_fmetapipe = -1;



void free_metabuf()
{
  if (g_metabuf.buf)
  {
		free (g_metabuf.buf);
		g_metabuf.buf = NULL;
		g_metabuf.pos = 0;
 	 	g_metabuf.size = META_PIPE_BUFFER_SIZE;
 	 }
}


int alloc_metabuf()
{
	if(g_metabuf.pos == g_metabuf.size)
    {
      if(g_metabuf.size <= META_PIPE_BUFFER_MAXSIZE)
        {
           DPRINTF(E_SPAM, L_FIFO , "Resize Metabuffer to %d!\n",g_metabuf.size + META_PIPE_BUFFER_SIZE);
          g_metabuf.buf = (char *) realloc(g_metabuf.buf, g_metabuf.size + META_PIPE_BUFFER_SIZE);
          g_metabuf.size += META_PIPE_BUFFER_SIZE;
          if (!g_metabuf.buf)
        	{
          	DPRINTF(E_LOG, L_FIFO, "Out of memory for metadata buffer\n");
          	return -1;
          }
        }
      else
       {
         DPRINTF(E_LOG, L_FIFO , "Max Buffer for Metabuffer reached, without scannable content. Discarding buffer!\n");
         free_metabuf();
       }
    }
    if(g_metabuf.buf== NULL)
    	{
    		g_metabuf.buf = (char*) malloc(META_PIPE_BUFFER_SIZE);
    		g_metabuf.pos = 0;
 	 			g_metabuf.size = META_PIPE_BUFFER_SIZE;
        if (!g_metabuf.buf)
        	{
          	DPRINTF(E_LOG, L_FIFO, "Out of memory for metadata buffer\n");
          	return -1;
          }
    	}
  return 0;
}


static void
audiopipe_read(evutil_socket_t fd, short event, void *arg)
{
  DPRINTF(E_LOG, L_FIFO , "Audio pipe ready to read\n");
  //uncomment to get notification until pipe is empty
  //struct event *ev = arg;
  //event_add(ev, NULL); 
}


int check_metadatatext (char *buf)
{  
  char delimiter[] = "\n";
  
  if (buf==NULL)
  {
  	DPRINTF(E_LOG, L_FIFO, "Metadata buffer is NULL"); 
  	return -1;
  }
  	
  char *ptr = strtok(buf, delimiter);
  
  while(ptr != NULL) 
  	{
  		uint32_t type,code,length;
			DPRINTF(E_SPAM, L_FIFO,"**  %.80s \n",ptr);
			int ret = sscanf(ptr,"<item><type>%8x</type><code>%8x</code><length>%u</length>",&type,&code,&length);
				DPRINTF(E_SPAM, L_FIFO, "code: %d ret:%d length: %d\n",code,ret,length);
				if (ret==3 && length != 0) 
					{
						ptr = strtok(NULL, delimiter);
						if (strcmp(ptr,"<data encoding=\"base64\">")!=0)
								{
									DPRINTF(E_LOG, L_FIFO, " Base64 tag <data encoding=\"base64\"> not seen , \"%.100s\" seen instead.\n",ptr); 
								}
							else
								{
									char *b64decoded;
									ptr = strtok(NULL, delimiter);
									char *endb64=strstr(ptr,"</data></item>");
									
									if (endb64==0)
										{
											DPRINTF(E_LOG, L_FIFO, "Metadata Pipe: End data tag not seen, \"%.80s\" seen instead.\n",ptr); 
											return -1;
										}
									 switch (code) {
										case 'asal':
											*endb64='=';// add an end tag for b64 encoding , as sometimes is is missing
										  b64decoded=b64_decode(ptr);
											DPRINTF(E_LOG, L_FIFO, "Metadata Pipe: Album Name: \"%s\".\n",b64decoded);
											free (b64decoded);
											break;
										case 'asar':
											*endb64='=';// add an end tag for b64 encoding , as sometimes is is missing
											b64decoded=b64_decode(ptr);
											DPRINTF(E_LOG, L_FIFO, "Metadata Pipe: Artist: \"%s\".\n",b64decoded);
											free (b64decoded);
											break;
										case 'minm':
											*endb64='=';// add an end tag for b64 encoding , as sometimes is is missing
											b64decoded=b64_decode(ptr);
											DPRINTF(E_LOG, L_FIFO, "Metadata Pipe: Title: \"%s\".\n",b64decoded);
											free (b64decoded);
											break;
										case 'PICT':
											DPRINTF(E_LOG, L_FIFO, "Metadata Pipe: Picture received, length %u bytes.\n",length);    
											break;               
								
									 }
								}	
					}
 			ptr = strtok(NULL, delimiter);
  }
  return 0;
}
  


static void
metadatapipe_read(evutil_socket_t fd, short event, void *arg)
{
  DPRINTF(E_SPAM, L_FIFO , "Metadata pipe ready to read\n");
  //char buf[65536];
  int len;
  struct event *ev = arg;
  struct timeval tv;

 	tv.tv_sec = TIMEOUT_PIPEREAD_SEC;
 	tv.tv_usec = 0;
 	
 	if (g_fmetapipe == -1)
   	{
      DPRINTF(E_LOG, L_FIFO, "Invalid file handle for metadata pipe\n");
      goto restart;
   	}
   
  
  
	if ((event & EV_TIMEOUT) && g_metabuf.buf) 
		{
			// check for scannable content ( either metadata or picture block) 
			if ((strstr(g_metabuf.buf, "<code>6d647374</code>")) && (strstr(g_metabuf.buf, "<code>6d64656e</code>")))
			 {
			  DPRINTF(E_SPAM, L_FIFO , "Metadata block found!\n");

			 }
		 if ((strstr(g_metabuf.buf, "<code>70637374</code>")) && (strstr(g_metabuf.buf, "<code>7063656e</code>")))
			 {
				DPRINTF(E_SPAM, L_FIFO , "Picture block found!\n");
			 }
			 
			if (check_metadatatext (g_metabuf.buf)!=0)
				{
					DPRINTF(E_LOG, L_FIFO , "********Error parsing metadata text!\n");
			  }
			free_metabuf();
  	} 
  else if (event & EV_READ) 
  	{
  		
  		if (g_metabuf.buf == NULL)
   		{
      	DPRINTF(E_LOG, L_FIFO, "No Metabuffer allocated\n");
      	alloc_metabuf();
   		}
   		
  		len = read(g_fmetapipe, g_metabuf.buf + g_metabuf.pos, g_metabuf.size - g_metabuf.pos);
  		if (len > 0)
   		{
				g_metabuf.pos += len;
				g_metabuf.buf[g_metabuf.pos]=0;
				DPRINTF(E_SPAM, L_FIFO , "Metadata Buffer Size: %d Buffer Pos: %d:\n",g_metabuf.size,g_metabuf.pos);
				alloc_metabuf();
			}
  	}
  
  restart:
   event_add(ev, &tv);
}

// Here we watch both audio and metadata pipe to become ready for read
static void *
pipewatcher(void *arg)
{
 struct stat sb;
 char * strAudioPipe=cfg_getstr(cfg_getsec(cfg, "pipe"), "audio_pipe");

 
  g_initialized = 1;
  
  DPRINTF(E_LOG, L_FIFO , "Pipewatcher thread is running!\n");
  DPRINTF(E_DBG, L_FIFO , "Setting up audio pipe: %s\n", strAudioPipe);

  if (lstat(strAudioPipe, &sb) < 0)
    {
      DPRINTF(E_LOG, L_FIFO , "Could not lstat() '%s': %s\n", strAudioPipe, strerror(errno));
      goto pipe_fail;
    }

  if (!S_ISFIFO(sb.st_mode))
    {
      DPRINTF(E_LOG, L_FIFO , "Source type is pipe, but path is not a fifo: %s\n", strAudioPipe);
      goto pipe_fail;
    }

  
  g_fpipe = open(strAudioPipe, O_RDONLY | O_NONBLOCK);
  if (g_fpipe < 0)
    {
      DPRINTF(E_LOG, L_FIFO, "Could not open pipe for reading '%s': %s\n", strAudioPipe, strerror(errno));
      goto audiopipe_fail;
    }
  else
    {
      g_pipewatcher_audioev = event_new(g_evbase_pipewatcher, g_fpipe, EV_READ , audiopipe_read,
                           event_self_cbarg());
      //Add it to the active events
     event_add(g_pipewatcher_audioev, NULL);
  }

 
  event_base_dispatch(g_evbase_pipewatcher);
  
  DPRINTF(E_LOG, L_FIFO , "Ending Pipewatcher thread\n");
  audiopipe_fail:
   close (g_fpipe);
   g_fpipe=-1;
  
  pipe_fail:
   if (g_initialized)
     {
       DPRINTF(E_LOG, L_FIFO , "Pipewatcher event loop terminated ahead of time!\n");
       g_initialized = 0;
     }

  pthread_exit(NULL);
}

//initializes pipe watching thread
int
pipewatcher_init()
{
  int ret;

  g_initialized = 0;

  g_evbase_pipewatcher = event_base_new();
  if (!g_evbase_pipewatcher)
    {
      DPRINTF(E_LOG, L_FIFO, "Could not create an event base for pipes\n");
      goto evbase_fail;
    }

  
  cmdbase = commands_base_new(g_evbase_pipewatcher, NULL);

  DPRINTF(E_INFO, L_PLAYER, "pipewatcher thread init\n");

  
  ret = pthread_create(&tid_pipewatcher, NULL, pipewatcher, NULL);
  if (ret < 0)
    {
      DPRINTF(E_LOG, L_PLAYER, "Could not spawn cache thread: %s\n", strerror(errno));

      goto thread_fail;
    }

  return 0;
  
 thread_fail:
  commands_base_free(cmdbase);
  event_base_free(g_evbase_pipewatcher);
  g_evbase_pipewatcher = NULL;

 evbase_fail:
  return -1;
}


//free pipe watching thread
void
pipewatcher_deinit(void)
{
  int ret;

  if (!g_initialized)
    return;

  g_initialized = 0;
  commands_base_destroy(cmdbase);

  ret = pthread_join(tid_pipewatcher, NULL);
  if (ret != 0)
    {
      DPRINTF(E_FATAL, L_FIFO, "Could not join pipe watcher thread: %s\n", strerror(errno));
      return;
    }

  // Free event base (should free events too)
  event_base_free(g_evbase_pipewatcher);
}



int
pipe_setup(const char *path)
{
  struct stat sb;
  if (!path)

    {
      DPRINTF(E_LOG, L_PLAYER, "Path to pipe is NULL\n");
      return -1;
    }

  DPRINTF(E_DBG, L_PLAYER, "Setting up pipe: %s\n", path);

  if (lstat(path, &sb) < 0)
    {
      DPRINTF(E_LOG, L_PLAYER, "Could not lstat() '%s': %s\n", path, strerror(errno));
      return -1;
    }

  if (!S_ISFIFO(sb.st_mode))
    {
      DPRINTF(E_LOG, L_PLAYER, "Source type is pipe, but path is not a fifo: %s\n", path);
      return -1;
    }

  pipe_cleanup();

  g_fd = open(path, O_RDONLY | O_NONBLOCK);
  if (g_fd < 0)
    {
      DPRINTF(E_LOG, L_PLAYER, "Could not open pipe for reading '%s': %s\n", path, strerror(errno));
      return -1;
    }

  g_buf = (uint16_t *)malloc(PIPE_BUFFER_SIZE);
  if (!g_buf)
    {
      DPRINTF(E_LOG, L_PLAYER, "Out of memory for buffer\n");
      return -1;
    }
  char strMetadataPipe[512];
  strncpy(strMetadataPipe,path,512);  
  strncat(strMetadataPipe,".metadata",512);
  DPRINTF(E_DBG, L_FIFO , "Setting up metadata pipe: %s\n", strMetadataPipe);
  
  if (lstat(strMetadataPipe, &sb) < 0)
    {
      DPRINTF(E_LOG, L_FIFO , "No metadata pipe found.%s \n", strMetadataPipe);
      return 0;
    }

  if (!S_ISFIFO(sb.st_mode))
    {
      DPRINTF(E_LOG, L_FIFO , "Source type is pipe, but path is not a fifo: %s\n", strMetadataPipe);
      return 0;
    }


  g_fmetapipe = open(strMetadataPipe, O_RDONLY | O_NONBLOCK);
  if (g_fmetapipe < 0)
    {
      DPRINTF(E_LOG, L_FIFO , "No metadata pipe found.%s \n", strMetadataPipe);
      return 0;
    }
  else
   {
    struct timeval tv;

 		tv.tv_sec = TIMEOUT_PIPEREAD_SEC;
 		tv.tv_usec = 0;
  
     //initialize buffer for metadata pipe
     free_metabuf();
     alloc_metabuf();

     g_pipewatcher_metadataev = event_new(g_evbase_pipewatcher, g_fmetapipe, EV_TIMEOUT | EV_READ , metadatapipe_read,
                           event_self_cbarg());
    //Add it to the active events, without a timeout 
     event_add(g_pipewatcher_metadataev, &tv);
   }

  return 0;
}

void
pipe_cleanup(void)
{
  
  DPRINTF(E_LOG, L_FIFO , "Cleaning up pipe...\n");
  if (g_fd >= 0)
    close(g_fd);
  g_fd = -1;

  if (g_fmetapipe!=-1)
   {
   	 event_del(g_pipewatcher_metadataev); 
     close (g_fmetapipe);
     g_fmetapipe=-1;
   }
  
  free_metabuf();
     
  if (g_buf)
    free(g_buf);
  g_buf = NULL;

  return;
}

int
pipe_audio_get(struct evbuffer *evbuf, int wanted)
{
  int got;

  if (wanted > PIPE_BUFFER_SIZE)
    wanted = PIPE_BUFFER_SIZE;

  got = read(g_fd, g_buf, wanted);

  if ((got < 0) && (errno != EAGAIN))
    {
      DPRINTF(E_LOG, L_PLAYER, "Could not read from pipe: %s\n", strerror(errno));
      return -1;
    }

  // If the other end of the pipe is not writing or the read was blocked,
  // we just return silence
  if (got <= 0)
    {
      memset(g_buf, 0, wanted);
      got = wanted;
    }

  evbuffer_add(evbuf, g_buf, got);

  return got;
}

