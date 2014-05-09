/*
 * shortest seek time first I/O scheduler
 */
#include <linux/blkdev.h>
#include <linux/elevator.h>
#include <linux/bio.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/init.h>

struct sstf_data {
	struct list_head queue;
	sector_t last_sect;
	struct list_head* next_dispatch;
	int queue_count;
};


static void sstf_print_list(struct request_queue *q)
{
	struct sstf_data *nd = q->elevator->elevator_data;

	struct list_head* pos_print;
	struct request* print_node;
	
	printk("Printing list: ");
	list_for_each(pos_print, &nd->queue) {
		print_node = list_entry(pos_print, struct request, queuelist);
		printk("%lu,", (unsigned long)blk_rq_pos(print_node));
	}

	printk("\n");
}

static void sstf_compare_seek(struct sstf_data *nd)
{	
	//get pointers to requests
	struct request* curr_request = list_entry(nd->next_dispatch, 
		struct request, queuelist);
	struct request* prev_request = list_entry(nd->next_dispatch->prev,
		struct request, queuelist);
	struct request* next_request = list_entry(nd->next_dispatch->next,
		struct request, queuelist);
	
	//get sectors
	unsigned long curr_sect = (unsigned long)blk_rq_pos(curr_request);
	unsigned long prev_sect = (unsigned long)blk_rq_pos(prev_request);
	unsigned long next_sect = (unsigned long)blk_rq_pos(next_request);

	//compare seek times
	unsigned long seek_prev = 0;
	unsigned long seek_next = 0;

	if(prev_sect > curr_sect) {
		seek_prev = prev_sect - curr_sect;
	}
	else if(prev_sect < curr_sect) {
		seek_prev = curr_sect - prev_sect;
	}
	else {
		seek_prev = 0;
	}

	if(next_sect > curr_sect) {
		seek_next = next_sect - curr_sect;
	}
	else if(next_sect < curr_sect) {
		seek_next = curr_sect - next_sect;
	}
	else {
		seek_next = 0;
	}

	printk("seek_prev = %lu, seek_next = %lu\n", seek_prev, seek_next);

	//Dispatch the task (prev or next) with shortest seek time
	if(seek_prev < seek_next) {
		printk("dispatching previous task\n");
		nd->next_dispatch = nd->next_dispatch->prev;
	}
	else {
		printk("dispatching next tast\n");
		nd->next_dispatch = nd->next_dispatch->next;
	}
}

static int sstf_merged_requests(struct request_queue *req_q, struct request *rq,
			 struct bio *bio)
{
	return ELEVATOR_NO_MERGE;
}

static int sstf_dispatch(struct request_queue *q, int force)
{
	printk("Dispatch\n");
	struct sstf_data *nd = q->elevator->elevator_data;

	if(!list_empty(&nd->queue)) {
		struct request *rq;
		rq = list_entry(nd->next_dispatch, struct request, queuelist);
	
		if(rq == 0) {
			printk("failed to dispatch");
			return 0;
		}

		if (nd->queue_count == 1) {
			printk("list contains only one item\n");
			list_del_init(&rq->queuelist);
			nd->queue_count--;
		}

		else {
			printk("dispatching multiple items\n");
			sstf_compare_seek(nd);
			
			//delete last request 
			list_del_init(&rq->queuelist);
			nd->queue_count--;
		}
		
		printk("Dispatching: %lu\n", (unsigned long)blk_rq_pos(rq));
		elv_dispatch_sort(q, rq);

		printk("Queue count: %d", nd->queue_count);

		return 1;
	}		
	
	return 0;
}

static void sstf_add_request(struct request_queue *q, struct request *rq)
{
	struct sstf_data *nd = q->elevator->elevator_data;
	int added = 0;
	
	printk("Adding %lu\n", (unsigned long)blk_rq_pos(rq));

	sstf_print_list(q);

	if(list_empty(&nd->queue)) {
		list_add(&rq->queuelist, &nd->queue);
		nd->next_dispatch = nd->queue.next;
		nd->queue_count++;
		
		sstf_print_list(q);
		return;
	}

	struct list_head* pos;
	list_for_each(pos, &nd->queue) {
		struct request* curr_request = list_entry(pos, struct request, queuelist);
		struct request* next_request = list_entry(pos->next, struct request, 
			queuelist);
		
		sector_t curr_sect = blk_rq_pos(curr_request);
		sector_t next_sect = blk_rq_pos(next_request);
		sector_t new_sect  = blk_rq_pos(rq);

		//if there's only one item in queue
		if(nd->queue_count == 1) {
			list_add(&rq->queuelist, pos);
			nd->queue_count++;
			added = 1;
			
			printk("Added second item\n");
			break;
		}

		//if there's multiple items in queue
		if( (next_sect >= new_sect) && (curr_sect <= new_sect) ) {
			list_add(&rq->queuelist, pos);
			nd->queue_count++;
			added = 1;
			
			printk("Adding item number 3 or higher\n");
			break;
		}
	}

	//request is larger than all current requests
	if(added != 1) {
		list_add_tail(&rq->queuelist, &nd->queue);
		nd->queue_count++;
		
		printk("added new item to tail\n");
	}

	printk("queue count: %d\n", nd->queue_count);
	sstf_print_list(q);
}

static void *sstf_init_queue(struct request_queue *q)
{
	struct sstf_data *nd;
	printk("Initializing queue");

	nd = kmalloc_node(sizeof(*nd), GFP_KERNEL, q->node);
	if(!nd) return NULL;

	INIT_LIST_HEAD(&nd->queue);
	nd->queue_count = 0;
	return nd;
}

static void sstf_exit_queue(struct elevator_queue *e)
{
	struct sstf_data *nd = e->elevator_data;

	BUG_ON(!list_empty(&nd->queue));
	kfree(nd);
}

static struct elevator_type elevator_sstf = {
	.ops = {
		.elevator_allow_merge_fn 	= sstf_merged_requests,
		.elevator_dispatch_fn		= sstf_dispatch,
		.elevator_add_req_fn		= sstf_add_request,
		.elevator_init_fn		= sstf_init_queue,
		.elevator_exit_fn		= sstf_exit_queue,
	},
	.elevator_name = "sstf",
	.elevator_owner = THIS_MODULE,
};

static int __init sstf_init(void)
{
	elv_register(&elevator_sstf);
	return 0;
}

static void __exit sstf_exit(void)
{
	elv_unregister(&elevator_sstf);
}

module_init(sstf_init);
module_exit(sstf_exit);

MODULE_AUTHOR("CS411 - Group 17");
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("SSTF IO scheduler");

