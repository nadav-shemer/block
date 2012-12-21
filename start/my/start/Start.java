/* Userspace code for reading blk_account's trace
 *
 * Copyright (C) 2012 by Nadav Shemer <nadav.shemer@gmail.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License version
 * 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; see the file COPYING.  If not, write to
 * the Free Software Foundation, 675 Mass Ave, Cambridge, MA 02139,
 * USA.
 *
 */
package my.start;

import java.io.BufferedInputStream;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileNotFoundException;
import java.io.FileWriter;
import java.io.IOException;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.util.Comparator;
import java.util.LinkedList;
import java.util.concurrent.ConcurrentSkipListMap;
import java.util.concurrent.ConcurrentSkipListSet;
import java.util.concurrent.atomic.AtomicInteger;

public class Start {
	
	private static enum Reason {
		REASON_READ, REASON_GET, REASON_DIRTY, REASON_ACCESSED, REASON_INACTIVE, REASON_ACTIVATED, REASON_EVICTED;
		
		public static Reason get(int reason){
			return Reason.values().length > reason && reason >= 0 ? Reason.values()[reason] : null;
		}
	}
	
	private static class Historeason {
		AtomicInteger read;
		AtomicInteger get;
		AtomicInteger dirty;
		AtomicInteger accessed;
		AtomicInteger inactive;
		AtomicInteger activated;
		AtomicInteger evicted;

		public Historeason() {
			this.read = new AtomicInteger();
			this.get = new AtomicInteger();
			this.dirty = new AtomicInteger();
			this.accessed = new AtomicInteger();
			this.inactive = new AtomicInteger();
			this.activated = new AtomicInteger();
			this.evicted = new AtomicInteger();
		}

		@Override
		public String toString() {
			return new StringBuilder()
			.append("read:").append(read.get())
			.append(" get:").append(get.get())
			.append(" dirty:").append(dirty.get())
			.append(" accessed:").append(accessed.get())
			.append(" inactive:").append(inactive.get())
			.append(" active:").append(activated.get())
			.append(" evicted:").append(evicted.get()).toString();
		}
	}

	private static void HistoryAddReason(Historeason h, Reason e){
		switch (e) {
			case REASON_ACCESSED:
				h.accessed.incrementAndGet();
				break;
			case REASON_ACTIVATED:
				h.activated.incrementAndGet();
				break;
			case REASON_DIRTY:
				h.dirty.incrementAndGet();
				break;
			case REASON_EVICTED:
				h.evicted.incrementAndGet();
				break;
			case REASON_GET:
				h.get.incrementAndGet();
				break;
			case REASON_INACTIVE:
				h.inactive.incrementAndGet();
				break;
			case REASON_READ:
				h.read.incrementAndGet();
				break;
		}
	}
	
	private static class BlockKey {
		final Long devid;
		final Long block;
		final Integer size;
		
		public BlockKey(Long devid, Long block, Integer size) {
			this.devid = devid;
			this.block = block;
			this.size = size;
		}
		
		@Override 
		public boolean equals(Object obj) {
			if (obj == null) return false;
			if (obj == this) return true;
			if ( !(obj instanceof BlockKey) ) return false;
			return (block_Comparator.compare((BlockKey)obj, this)==0);
		}
		
		@Override 
		public int hashCode() {
			int hash = devid.hashCode();
			hash = hash * 31 + block.hashCode();
			return (hash * 31 + size.hashCode());
		}
	}
	
	private static class PageKey {
		final Long devid;
		final Long i_no;
		final Integer pgindex;
		
		public PageKey(Long devid, Long i_no, Integer pgindex) {
			this.devid = devid;
			this.i_no = i_no;
			this.pgindex = pgindex;
		}

		@Override 
		public boolean equals(Object obj) {
			if (obj == null) return false;
			if (obj == this) return true;
			if ( !(obj instanceof PageKey) ) return false;
			return (page_Comparator.compare((PageKey)obj, this)==0);
		}
		
		@Override 
		public int hashCode() {
			int hash = devid.hashCode();
			hash = hash * 31 + i_no.hashCode();
			return (hash * 31 + pgindex.hashCode());
		}
	}
	private static class PageHistoryData {
		final PageKey page;
		final String devname;
		final ConcurrentSkipListSet<BlockKey> blocks;
		final Historeason counters;

		public PageHistoryData(PageKey page, String devname) {
			this.page = page;
			this.devname = devname;
			this.blocks = new ConcurrentSkipListSet<>(block_Comparator);
			this.counters = new Historeason();
		}
	}
	private static class BlockHistoryData {
		final BlockKey block;
		final String devname;
		final ConcurrentSkipListSet<PageKey> pages;
		final Historeason counters;

		public BlockHistoryData(BlockKey block, String devname) {
			this.block = block;
			this.devname = devname;
			this.pages = new ConcurrentSkipListSet<>(page_Comparator);
			this.counters = new Historeason();
		}
	}
	
	private static class Record {
		long jiffies;
		long i_no;
		long block;
		/** unsigned int */
		long devid; 
		/** unsigned int */
		long pgdevid; 
		int pgindex;
		int size;
		int pid;
		int tgid;
		int reason;
		String devname;
		String procname;
		BlockKey block_key;
		PageKey page_key;
		
		public void make_keys() {
			if (block_key == null && block != 0) {
				block_key = new BlockKey(devid, block, size);
			}
			if (page_key == null && i_no != 0) {
				page_key = new PageKey(pgdevid, i_no, pgindex);
			}
		}
	}
	
	private static Record RecordReader(byte[] inb) throws IOException
	{
		Record record = new Record();
		if (inb.length != 100) return null;
		ByteBuffer b = java.nio.ByteBuffer.wrap(inb);
		b.order(ByteOrder.LITTLE_ENDIAN);
		record.jiffies = b.getLong(); //8
		record.i_no = b.getLong(); //16
		record.block = b.getLong(); //24
		record.devid = b.getInt(); //28
		record.pgdevid = b.getInt(); //32
		record.pgindex = b.getInt(); //36
		record.size = b.getInt(); //40
		record.pid = b.getInt(); //44        
		record.tgid = b.getInt(); //48
		record.reason = b.getInt(); //52           
		StringBuilder s = new StringBuilder(16);
		int found_end = 0;
		for (int i = 0; i < 16; i++) {
			char c = (char)b.get();
			if (found_end == 1)
				continue;
			if (c != 0)
				s.append(c);
			else found_end = 1;
		}
		record.procname = s.toString();
		s = new StringBuilder(32);
		found_end = 0;
		for (int i = 0; i < 32; i++) {
			char c = (char)b.get();
			if (found_end == 1)
				continue;
			if (c != 0)
				s.append(c);
			else found_end = 1;
		}
		record.devname = s.toString();
		record.make_keys();
		return record;
	}
	
	private static Record RecordGetter(BufferedInputStream in_stream) throws IOException
	{
		byte[] buf = new byte[100];
		int res = in_stream.read(buf);
		switch (res) {
			case 100:
				return RecordReader(buf);
			case 0:
				return null;
		}
		return null;
	}
	
	private static class Block_Comparator implements Comparator<BlockKey> {
		@Override
		public int compare(BlockKey o1, BlockKey o2) {
			int a = o1.devid.compareTo(o2.devid);
			if (a == 0) {
				int b = o1.block.compareTo(o2.block);
				if (b == 0)
					return o1.size.compareTo(o2.size);
				return b;
			}
			return a;
		}
	}

	private static class Page_Comparator implements Comparator<PageKey> {
		@Override
		public int compare(PageKey o1, PageKey o2) {
			int a = o1.devid.compareTo(o2.devid);
			if (a == 0) {
				int b = o1.i_no.compareTo(o2.i_no);
				if (b == 0)
					return o1.pgindex.compareTo(o2.pgindex);
				return b;
			}
			return a;
		}
	}
	
	
	private static final Block_Comparator block_Comparator = new Block_Comparator();
	private static final Page_Comparator page_Comparator = new Page_Comparator();


	private static final ConcurrentSkipListMap<BlockKey,BlockHistoryData> block_map = new ConcurrentSkipListMap<>(block_Comparator);
	private static final ConcurrentSkipListMap<PageKey,PageHistoryData> page_map = new ConcurrentSkipListMap<>(page_Comparator);

	private static class RecordParser implements Runnable {
		File inf;
		BufferedInputStream inb;
		
		public RecordParser(File f) throws FileNotFoundException {
			inf = f;
			inb = new BufferedInputStream(new FileInputStream(inf), 8000);
		}

		@Override
		public void run() {
			Record r;
			while (true) {
				try {
					r = RecordGetter(inb);
					if (r != null) {
						if (r.devname.compareTo("sda1") != 0)
							continue;
//						System.out.println("Record time:" + r.jiffies + " block=" + r.devid + ":" + r.block + ":" + r.size + " page=" + r.pgdevid + ":" + r.i_no + ":" + r.pgindex + " pid:" + r.pid + ":" + r.tgid + " reason:" + int_to_reason(r.reason).toString() + " proc:" + r.procname + "\n");
////						System.out.println("Keys " + r.block_key + " : " + r.page_key + "\n");
////						if (r.block_key != null)System.out.println("Block key " + r.block_key.devid + ":" + r.block_key.block + ":" + r.block_key.size + "\n");
////						if (r.page_key != null)System.out.println("Page key " + r.page_key.devid + ":" + r.page_key.i_no + ":" + r.page_key.pgindex + "\n");
						if (r.block_key != null) {
							BlockHistoryData b = block_map.get(r.block_key);
							if (b == null) {
								b = new BlockHistoryData(r.block_key, r.devname);
								BlockHistoryData o = block_map.putIfAbsent(r.block_key, b);
								if (o != null)
									b = o;
							}
							HistoryAddReason(b.counters, Reason.get(r.reason));
							if (r.page_key != null)
								b.pages.add(r.page_key);
						}
						if (r.page_key != null) {
							PageHistoryData p = page_map.get(r.page_key);
							if (p == null) {
								p = new PageHistoryData(r.page_key, r.devname);
								PageHistoryData o = page_map.putIfAbsent(r.page_key, p);
								if (o != null)
									p = o;
							}
							if (r.block_key == null)
								HistoryAddReason(p.counters, Reason.get(r.reason));
							else
								p.blocks.add(r.block_key);
						}
					}
				} catch (IOException e) {
					System.out.println("Cant read record " + e.getMessage() + "\n");
					System.exit(1);
				}
			}
		}
	}

	private static void print_block_data(FileWriter w, BlockHistoryData h) throws IOException {
		w.write("BlockHistory " + h.devname + " block:" + h.block.devid + ":" + h.block.block + ":" + h.block.size + "\n");
		for (PageKey page:h.pages) {
			w.write("BlockHistory page:" + page.devid + ":" + page.i_no + ":" + page.pgindex + "\n");
		}
		w.write("BlockHistory " + h.counters.toString() + "\n");
	}

	private static void print_page_data(FileWriter w, PageHistoryData h) throws IOException {
		Historeason c = new Historeason();
		w.write("PageHistory " + h.devname + " page:" + h.page.devid + ":" + h.page.i_no + ":" + h.page.pgindex + "\n");
		for (BlockKey block:h.blocks) {
			BlockHistoryData b = block_map.get(block);
			if (b != null) {
				c.read.addAndGet(b.counters.read.get());
				c.get.addAndGet(b.counters.get.get());
				c.dirty.addAndGet(b.counters.dirty.get());
				c.accessed.addAndGet(b.counters.accessed.get());
				c.inactive.addAndGet(b.counters.inactive.get());
				c.activated.addAndGet(b.counters.activated.get());
				c.evicted.addAndGet(b.counters.evicted.get());
			}
			w.write("PageHistory block:" + block.devid + ":" + block.block + ":" + block.size + "\n");
		}
		w.write("PageHistory " + h.counters.toString() + "\n");
		w.write("PageHistory blocks " + c.toString() + "\n");
	}

	/**
	 * @param args
	 */
	public static void main(String[] args) {
		System.out.println("Hello world");
		int i = 0;
		
		while (true) {
			String infname = new String("/proc/bp/kbpd" + i);
			String tname = new String("Read"+i);
			File inf = new File(infname);
			try {
				RecordParser inr = new RecordParser(inf);
				Thread t = new Thread(inr, tname); 
				t.start();
			} catch (FileNotFoundException e) {
				System.out.println("Cant open file " + e.getMessage());
				break;
			}
			i++;
		}
		while (true) {
			try {
				Thread.sleep(60000);
			} catch (InterruptedException e1) {
				System.out.println("Interrupted " + e1.getMessage());
			}
			FileWriter w;
			try {
				w = new FileWriter("/remote/mnt/raid/workspace/hist_data");
				for(BlockHistoryData h:block_map.values()) {
					print_block_data(w, h);
				}
				for(PageHistoryData h:page_map.values()) {
					print_page_data(w, h);
				}
				w.close();
			} catch (IOException e) {
				System.out.println("Cant open file " + e.getMessage());
				System.exit(1);
			}

		}
	}
}
