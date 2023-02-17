/*-
 *   BSD LICENSE
 *
 *   Copyright (c) Intel Corporation.
 *   All rights reserved.
 *
 *   Redistribution and use in source and binary forms, with or without
 *   modification, are permitted provided that the following conditions
 *   are met:
 *
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in
 *       the documentation and/or other materials provided with the
 *       distribution.
 *     * Neither the name of Intel Corporation nor the names of its
 *       contributors may be used to endorse or promote products derived
 *       from this software without specific prior written permission.
 *
 *   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 *   "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 *   LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 *   A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 *   OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 *   SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 *   LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 *   DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 *   THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 *   (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 *   OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "spdk/stdinc.h"
#include "spdk/thread.h"
#include "spdk/bdev.h"
#include "spdk/bdev_module.h"
#include "spdk/env.h"
#include "spdk/event.h"
#include "spdk/log.h"
#include "spdk/string.h"
#include "spdk/bdev_zone.h"

/*
#include "spdk/config.h"
#include "spdk/likely.h"
#include "spdk/queue.h"
#include "spdk/nvme_spec.h"
#include "spdk/scsi_spec.h"
#include "spdk/notify.h"
#include "spdk/util.h"
#include "spdk/trace.h"
//#include "spdk_internal/thread.h"
*/

/*
struct spdk_bdev;
struct spdk_bdev_io;
struct spdk_bdev_channel;
void bdev_io_submit(struct spdk_bdev_io *bdev_io);
void bdev_io_init(struct spdk_bdev_io *bdev_io, struct spdk_bdev *bdev, void *cb_arg,
		spdk_bdev_io_completion_cb cb);
struct spdk_bdev_io *bdev_channel_get_io(struct spdk_bdev_channel *channel);
*/

static char *g_bdev_name = "Nvme0";
int spdk_i_i = 0,rr_flag = 0;
/*
 * We'll use this struct to gather housekeeping hello_context to pass between
 * our events and callbacks.
 */
struct hello_context_t {
	struct spdk_bdev *bdev;
	struct spdk_bdev_desc *bdev_desc;
	struct spdk_io_channel *bdev_io_channel;
	char *buff;
	char *bdev_name;
	struct spdk_bdev_io_wait_entry bdev_io_wait;
};

/*
 * Usage function for printing parameters that are specific to this application
 */
static void
hello_bdev_usage(void)
{
	printf(" -b <bdev>                 name of the bdev to use\n");
}

/*
 * This function is called to parse the parameters that are specific to this application
 */
static int hello_bdev_parse_arg(int ch, char *arg)
{
	switch (ch) {
	case 'b':
		g_bdev_name = arg;
		break;
	default:
		return -EINVAL;
	}
	return 0;
}

/*

int spdk_bdev_search(struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
		void *buf, uint64_t offset, uint64_t nbytes,
		spdk_bdev_io_completion_cb cb, void *ch_arg)
{
	uint64_t offset_blocks, num_blocks;

	if (bdev_bytes_to_blocks(spdk_bdev_desc_get_bdev(desc), offset, &offset_blocks,
				nbytes, &num_blocks)!= 0) {
		return -EINVAL;
	}
	
	return spdk_bdev_search_blocks_md(desc, ch, buf, NULL, offset_blocks, num_blocks, cb, ch_arg);
}


int spdk_bdev_search_blocks_md(struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
		void *buf, void *md_buf, uint64_t offset_blocks, uint64_t num_blocks,
		spdk_bdev_io_completion_cb cb, void *ch_arg)
{	
	struct spdk_bdev *bdev = spdk_bdev_desc_get_bdev(desc);
	struct spdk_bdev_io *bdev_io;
	struct spdk_bdev_channel *channel = spdk_io_channel_get_ctx(ch);

	if(!desc->write){
		return -EBADF;
	}
	if(!bdev_io_valid_blocks(bdev, offset_blocks, num_blocks)) {
		return -EINVAL;
	}
	
	bdev_io = bdev_channel_get_io(channel);
	if(!bdev_io){
		return -ENOMEM;
	}

	bdev_io->internal.ch = channel;
	bdev_io->internal.desc = desc;
	bdev_io->type = SPDK_BDEV_NUM_IO_TYPES + 1;
	bdev_io->u.bdev.iovs = &bdev_io->iov;
	bdev_io->u.bdev.iovs[0].iov_base = buf;
	bdev_io->u.bdev.iovs[0].iov_len = num_blocks * 512;
	bdev_io->u.bdev.iovcnt = 1;
	bdev_io->u.bdev.md_buf = md_buf;
	bdev_io->u.bdev.num_blocks = num_blocks;
	bdev_io->u.bdev.offset_blocks = offset_blocks;
	bdev_io_init(bdev_io, bdev, ch_arg, cb);

	bdev_io_submit(bdev_io);
	return 0;

}

*/
/*
 * Callback function for read io completion.
 */
static void
read_complete(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg)
{
	struct hello_context_t *hello_context = cb_arg;

	if (success) {
		SPDK_NOTICELOG("Read string from bdev : %s\n", hello_context->buff+(rr_flag*512));
		printf("rr_flags = %d\n", rr_flag);
		if(strcmp(hello_context->buff+(rr_flag*512),"hello world!4")==0)
			printf("%s find\n",hello_context->buff+(rr_flag*512));
		for(int i=0;i<13;i++){
			printf("%c", *(hello_context->buff+(rr_flag*512)+i));
		}
		printf("\n");
	} else {
		SPDK_ERRLOG("bdev io read error\n");
	}
	if(rr_flag != 0){
		rr_flag++;
	}
	/* Complete the bdev io and close the channel */
	else if(rr_flag == 0){
	spdk_bdev_free_io(bdev_io);
	spdk_put_io_channel(hello_context->bdev_io_channel);
	spdk_bdev_close(hello_context->bdev_desc);
	SPDK_NOTICELOG("Stopping app\n");
	spdk_app_stop(success ? 0 : -1);
	}
}

static void
hello_read(void *arg)
{
	struct hello_context_t *hello_context = arg;
	int rc = 0,i=0;
	uint32_t length = spdk_bdev_get_block_size(hello_context->bdev);

	SPDK_NOTICELOG("Reading io\n");
	//printf("length:%u\n",length);
//	rc = spdk_bdev_read(hello_context->bdev_desc, hello_context->bdev_io_channel,
//				hello_context->buff, 0, length, read_complete, hello_context);

//	for(i=0;i<9;i++){
		printf("count:%d\n",i);
		rc = spdk_bdev_read(hello_context->bdev_desc, hello_context->bdev_io_channel,
			    hello_context->buff+(i*512),i*512, length, read_complete, hello_context);
	
		if (rc == -ENOMEM) {
		SPDK_NOTICELOG("Queueing io\n");
		/* In case we cannot perform I/O now, queue I/O */
		hello_context->bdev_io_wait.bdev = hello_context->bdev;
		hello_context->bdev_io_wait.cb_fn = hello_read;
		hello_context->bdev_io_wait.cb_arg = hello_context;
		spdk_bdev_queue_io_wait(hello_context->bdev, hello_context->bdev_io_channel,
					&hello_context->bdev_io_wait);
	} else if (rc) {
		SPDK_ERRLOG("%s error while reading from bdev: %d\n", spdk_strerror(-rc), rc);
		spdk_put_io_channel(hello_context->bdev_io_channel);
		spdk_bdev_close(hello_context->bdev_desc);
		spdk_app_stop(-1);
	}
//	}
}

/*
 * Callback function for write io completion.
 */
static void
write_complete(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg)
{
	struct hello_context_t *hello_context = cb_arg;
	uint32_t length;

	/* Complete the I/O */
	spdk_bdev_free_io(bdev_io);

	if (success) {
		SPDK_NOTICELOG("bdev io write completed successfully\n");
	} else {
		SPDK_ERRLOG("bdev io write error: %d\n", EIO);
		spdk_put_io_channel(hello_context->bdev_io_channel);
		spdk_bdev_close(hello_context->bdev_desc);
		spdk_app_stop(-1);
		return;
	}

	/* Zero the buffer so that we can use it for reading */
	length = spdk_bdev_get_block_size(hello_context->bdev);
	memset(hello_context->buff, 0, length);
	if(rr_flag != 0){
		rr_flag ++;
	}
	else if(rr_flag == 0){
		hello_read(hello_context);
		rr_flag = 0;
	}
}

static void
hello_write(void *arg)
{
	struct hello_context_t *hello_context = arg;
	char buf1[512];
	int rc = 0,i=0;
	uint32_t length = spdk_bdev_get_block_size(hello_context->bdev),buf_align;

	SPDK_NOTICELOG("4. Writing to the bdev\n");
		
	//rc = spdk_bdev_write(hello_context->bdev_desc, hello_context->bdev_io_channel,
	//		     hello_context->buff, 0, length, write_complete, hello_context);
	//buf_align = spdk_bdev_get_buf_align(hello_context->bdev);
	//buf1 = spdk_dma_zmalloc(length*10, buf_align, NULL);
	printf("검색어를 입력하세요:");
	scanf("%s",buf1);
	printf("%s,,",buf1);	
	snprintf(hello_context->buff, length, buf1);
	// 새로운 함수 이전거 
	//rc = spdk_bdev_write(hello_context->bdev_desc, hello_context->bdev_io_channel,
	//		     hello_context->buff, 0, length, write_complete, hello_context);
				 //
	rc = spdk_bdev_search(hello_context->bdev_desc, hello_context->bdev_io_channel,
			hello_context->buff, 0, length, write_complete, hello_context);
	/*
	for(i=0;i<9;i++){
		
		printf("hello world %d\n",i);
		if(i==0){
			snprintf(hello_context->buff+(i*512), length, "hello_world!%d",i);
			rc = spdk_bdev_write(hello_context->bdev_desc, hello_context->bdev_io_channel,
					hello_context->buff+(i*512), i*512, length, write_complete, hello_context);
		}
		else{
			snprintf(hello_context->buff+(i*512), length, "hello world!%d",i);
			rc = spdk_bdev_write(hello_context->bdev_desc, hello_context->bdev_io_channel,
				hello_context->buff+(i*512), i*512 ,length, write_complete, hello_context); 
		}
		*/
	if (rc == -ENOMEM) {
		SPDK_NOTICELOG("Queueing io\n");
		/* In case we cannot perform I/O now, queue I/O */
		hello_context->bdev_io_wait.bdev = hello_context->bdev;
		hello_context->bdev_io_wait.cb_fn = hello_write;
		hello_context->bdev_io_wait.cb_arg = hello_context;
		spdk_bdev_queue_io_wait(hello_context->bdev, hello_context->bdev_io_channel,
					&hello_context->bdev_io_wait);
	} else if (rc) {
		SPDK_ERRLOG("%s error while writing to bdev: %d\n", spdk_strerror(-rc), rc);
		spdk_put_io_channel(hello_context->bdev_io_channel);
		spdk_bdev_close(hello_context->bdev_desc);
		spdk_app_stop(-1);
	}
	//}
}

static void
hello_bdev_event_cb(enum spdk_bdev_event_type type, struct spdk_bdev *bdev,
		    void *event_ctx)
{
	SPDK_NOTICELOG("Unsupported bdev event: type %d\n", type);
}

static void
reset_zone_complete(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg)
{
	struct hello_context_t *hello_context = cb_arg;

	/* Complete the I/O */
	spdk_bdev_free_io(bdev_io);

	if (!success) {
		SPDK_ERRLOG("bdev io reset zone error: %d\n", EIO);
		spdk_put_io_channel(hello_context->bdev_io_channel);
		spdk_bdev_close(hello_context->bdev_desc);
		spdk_app_stop(-1);
		return;
	}

	hello_write(hello_context);
}

static void
hello_reset_zone(void *arg)
{
	struct hello_context_t *hello_context = arg;
	int rc = 0;

	rc = spdk_bdev_zone_management(hello_context->bdev_desc, hello_context->bdev_io_channel,
				       0, SPDK_BDEV_ZONE_RESET, reset_zone_complete, hello_context);

	if (rc == -ENOMEM) {
		SPDK_NOTICELOG("Queueing io\n");
		/* In case we cannot perform I/O now, queue I/O */
		hello_context->bdev_io_wait.bdev = hello_context->bdev;
		hello_context->bdev_io_wait.cb_fn = hello_reset_zone;
		hello_context->bdev_io_wait.cb_arg = hello_context;
		spdk_bdev_queue_io_wait(hello_context->bdev, hello_context->bdev_io_channel,
					&hello_context->bdev_io_wait);
	} else if (rc) {
		SPDK_ERRLOG("%s error while resetting zone: %d\n", spdk_strerror(-rc), rc);
		spdk_put_io_channel(hello_context->bdev_io_channel);
		spdk_bdev_close(hello_context->bdev_desc);
		spdk_app_stop(-1);
	}
}

/*
 * Our initial event that kicks off everything from main().
 */
static void
hello_start(void *arg1)
{
	struct hello_context_t *hello_context = arg1;
	char *buf1 = NULL;
	uint32_t blk_size, buf_align;
	int rc = 0;
	hello_context->bdev = NULL;
	hello_context->bdev_desc = NULL;

	SPDK_NOTICELOG("2.Successfully started the application\n");

	/*
	 * There can be many bdevs configured, but this application will only use
	 * the one input by the user at runtime.
	 *
	 * Open the bdev by calling spdk_bdev_open_ext() with its name.
	 * The function will return a descriptor
	 */
	SPDK_NOTICELOG("Opening the bdev %s\n", hello_context->bdev_name);
	rc = spdk_bdev_open_ext(hello_context->bdev_name, true, hello_bdev_event_cb, NULL,
				&hello_context->bdev_desc);
	if (rc) {
		SPDK_ERRLOG("Could not open bdev: %s\n", hello_context->bdev_name);
		spdk_app_stop(-1);
		return;
	}

	/* A bdev pointer is valid while the bdev is opened. */
	hello_context->bdev = spdk_bdev_desc_get_bdev(hello_context->bdev_desc);


	SPDK_NOTICELOG("Opening io channel\n");
	/* Open I/O channel */
	hello_context->bdev_io_channel = spdk_bdev_get_io_channel(hello_context->bdev_desc);
	//printf("channel is : %s\n",spdk_io_channel_get_io_device_name(hello_context->bdev_io_channel));
	if (hello_context->bdev_io_channel == NULL) {
		SPDK_ERRLOG("Could not create bdev I/O channel!!\n");
		spdk_bdev_close(hello_context->bdev_desc);
		spdk_app_stop(-1);
		return;
	}

	/* Allocate memory for the write buffer.
	 * Initialize the write buffer with the string "Hello World!"
	 */
	blk_size = spdk_bdev_get_block_size(hello_context->bdev);
	buf_align = spdk_bdev_get_buf_align(hello_context->bdev);
	hello_context->buff = spdk_dma_zmalloc(blk_size*10, buf_align, NULL);
//	buf1 = spdk_dma_zmalloc(blk_size, buf_align, NULL);
	if (!hello_context->buff) {
		SPDK_ERRLOG("Failed to allocate buffer\n");
		spdk_put_io_channel(hello_context->bdev_io_channel);
		spdk_bdev_close(hello_context->bdev_desc);
		spdk_app_stop(-1);
		return;
	}
	//snprintf(hello_context->buff, blk_size, "%s", "my");

	if (spdk_bdev_is_zoned(hello_context->bdev)) {
		hello_reset_zone(hello_context);
		/* If bdev is zoned, the callback, reset_zone_complete, will call hello_write() */
		return;
	}
	SPDK_NOTICELOG("3. hello_write 진입\n");

	hello_write(hello_context);
	//hello_read(hello_context);	
	//SPDK_NOTICELOG("돌아왔음!\n");	
	//spdk_bdev_free_io(bdev_io);
	//spdk_put_io_channel(hello_context->bdev_io_channel);
	//spdk_bdev_close(hello_context->bdev_desc);
	//SPDK_NOTICELOG("하나 처리 완료!\n");
	//spdk_app_stop(0);
}

int
main(int argc, char **argv)
{
	struct spdk_app_opts opts = {};
	int rc = 0;
	struct hello_context_t hello_context = {};

	/* Set default values in opts structure. */
	spdk_app_opts_init(&opts, sizeof(opts));
	opts.name = "hello_bdev";

	/*
	 * Parse built-in SPDK command line parameters as well
	 * as our custom one(s).
	 */
	if ((rc = spdk_app_parse_args(argc, argv, &opts, "b:", NULL, hello_bdev_parse_arg,
				      hello_bdev_usage)) != SPDK_APP_PARSE_ARGS_SUCCESS) {
		exit(rc);
	}
	hello_context.bdev_name = g_bdev_name;
	printf("g_bdev_name : %s,%s\n",hello_context.bdev_name,g_bdev_name);
	/*
	 * spdk_app_start() will initialize the SPDK framework, call hello_start(),
	 * and then block until spdk_app_stop() is called (or if an initialization
	 * error occurs, spdk_app_start() will return with rc even without calling
	 * hello_start().
	 */
	rc = spdk_app_start(&opts, hello_start, &hello_context);
	if (rc) {
		SPDK_ERRLOG("ERROR starting application\n");
	}
	printf("finfinfin\n");
	/* At this point either spdk_app_stop() was called, or spdk_app_start()
	 * failed because of internal error.
	 */

	/* When the app stops, free up memory that we allocated. */
	spdk_dma_free(hello_context.buff);

	/* Gracefully close out all of the SPDK subsystems. */
	spdk_app_fini();
	return rc;
}
