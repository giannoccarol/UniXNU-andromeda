#include <stdint.h>
#include <stddef.h>
#include <assert.h>
#include <stdio.h>
#include "cepheus_hfi.h"
struct mock { unsigned acquires, publishes, notifies, data_publishes; int fail, fail_data, fail_index, fail_notify; uint32_t *write_index; uint32_t before_index; };
static int acquire(void *p) { ((struct mock *)p)->acquires++; return ((struct mock *)p)->fail; }
static int publish_data(void *p) { struct mock *m=p; m->data_publishes++; if(m->write_index) assert(*m->write_index==m->before_index); return m->fail_data; }
static int publish(void *p) { ((struct mock *)p)->publishes++; return ((struct mock *)p)->fail_index; }
static int notify(void *p) { ((struct mock *)p)->notifies++; return ((struct mock *)p)->fail_notify; }
int main(void)
{
	uint32_t data[16] = {0}, r = 12, w = 12, out[8]; unsigned n;
	struct mock m = {0};
	struct ceph_hfi_queue q = {data,&r,&w,16,2,{&m,acquire,publish_data,publish,notify},0};
	unsigned ir,iw;assert(ceph_hfi_indices(&q,NULL,&iw)==CE_HFI_INVALID && ceph_hfi_indices(&q,&ir,NULL)==CE_HFI_INVALID);
	struct ceph_hfi_queue misaligned=q;misaligned.write_index=(uint32_t *)((uint8_t *)&w+1);
	assert(ceph_hfi_indices(&misaligned,&ir,&iw)==CE_HFI_INVALID);
	uint32_t msg[] = {ceph_hfi_header(1, 3, 0, 4095), 0x1111, 0x2222};
	assert(ceph_hfi_write(&q,msg,3)==0 && w==0 && data[15]==0xfafafafa);
	assert(m.publishes==1 && m.notifies==1 && m.data_publishes==1);
	assert(ceph_hfi_read(&q,out,2,&n)==CE_HFI_SMALL && r==12 && n==0);
	assert(!ceph_hfi_read(&q,out,8,&n) && n==3 && r==0 && out[2]==0x2222);
	assert(ceph_hfi_read(&q,out,8,&n)==CE_HFI_EMPTY);
	for (unsigned i=0;i<3;i++) { assert(!ceph_hfi_write(&q,msg,3)); }
	assert(ceph_hfi_write(&q,msg,3)==CE_HFI_FULL && w==12);
	r=16; assert(ceph_hfi_write(&q,msg,3)==CE_HFI_INVALID); r=0;
	data[0]=ceph_hfi_header(1,15,0,0); assert(ceph_hfi_read(&q,out,8,&n)==CE_HFI_INVALID);
	data[0]=0; assert(ceph_hfi_read(&q,out,8,&n)==CE_HFI_INVALID);
	m.fail=1; assert(ceph_hfi_write(&q,msg,3)==CE_HFI_IO); m.fail=0;
	uint32_t ack[]={ceph_hfi_header(1,3,1,0),msg[0],0};
	assert(ceph_hfi_ack(ack,3,msg[0])); ack[1]^=1; assert(!ceph_hfi_ack(ack,3,msg[0]));
	ack[1]=msg[0]; ack[2]=1; assert(!ceph_hfi_ack(ack,3,msg[0]));
	assert(!ceph_hfi_header(1,256,0,0)); assert(!ceph_hfi_header(1,0,0,0));
	uint32_t table[54]; assert(!ceph_hfi_table(table,54,0x60000000,20480));
	assert(table[7]==0x600000e0 && table[42]==0 && table[45]==1024);
	assert(ceph_hfi_table(table,54,0xfffff000u,20480)==CE_HFI_INVALID);
	assert(ceph_hfi_table(table,53,0x60000000,20480)==CE_HFI_INVALID);
	r=w=0;data[0]=msg[0];data[1]=msg[1];data[2]=msg[2];
	assert(ceph_hfi_write(&q,data,3)==CE_HFI_INVALID && w==0);
	assert(!ceph_hfi_write(&q,msg,3));
	assert(ceph_hfi_read(&q,data,8,&n)==CE_HFI_INVALID && r==0 && n==0);
	/* Never expose a new write index before data publication; any publication
	 * or doorbell failure poisons the queue instead of duplicating commands. */
	for(unsigned failure=1; failure<=3; ++failure) {
		r=w=0; m=(struct mock){.write_index=&w,.before_index=0,
		    .fail_data=failure==1,.fail_index=failure==2,.fail_notify=failure==3};
		q.poisoned=0;
		assert(ceph_hfi_write(&q,msg,3)==CE_HFI_IO && q.poisoned);
		assert(w==(failure==1 ? 0u : 4u));
		assert(m.publishes==(failure==1 ? 0u : 1u));
		assert(m.notifies==(failure==3 ? 1u : 0u));
		unsigned calls=m.acquires;
		assert(ceph_hfi_write(&q,msg,3)==CE_HFI_POISONED && m.acquires==calls);
		assert(ceph_hfi_read(&q,out,8,&n)==CE_HFI_POISONED && n==0);
	}
	r=w=0; q.poisoned=0; m=(struct mock){.write_index=&w,.before_index=0};
	assert(!ceph_hfi_write(&q,msg,3));
	m.fail_index=1;
	assert(ceph_hfi_read(&q,out,8,&n)==CE_HFI_IO && n==0 && q.poisoned);
	assert(ceph_hfi_read(&q,out,8,&n)==CE_HFI_POISONED);
	puts("HFI ring wrap/full/partial/ACK/table/cache callbacks: PASS (mock, no GMU)");
}
