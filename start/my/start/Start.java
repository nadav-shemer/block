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
import java.io.IOException;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.util.Comparator;
import java.util.LinkedList;
import java.util.Map;
import java.util.TreeMap;
import java.util.concurrent.ConcurrentSkipListMap;

public class Start {
	private static class BlockKey {
		Long devid;
		Long block;
		Integer size;
		@Override public boolean equals(Object obj) {
			if (obj == null)
				return false;
			if ( !(obj instanceof BlockKey) ) return false;
			BlockKey key = (BlockKey)obj;
			return (devid.equals(key.devid) && block.equals(key.block) && size.equals(key.size) );
		}
		@Override public int hashCode() {
			int hash = devid.hashCode();
			hash = hash * 31 + block.hashCode();
			return (hash * 31 + size.hashCode());
		}
	}
	private static class PageKey {
		Long devid;
		Long i_no;
		Integer pgindex;
	}
	private static class Record {
		long jiffies;
		long i_no;
		long block;
		long devid; /* unsigned int */
		long pgdevid; /* unsigned int */
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
			if (block_key == null) {
				if (block != 0) {
					block_key = new BlockKey();
					block_key.devid = devid;
					block_key.block = block;
					block_key.size = size;
				}
			}
			if (page_key == null) {
				if (i_no != 0) {
					page_key = new PageKey();
					page_key.devid = pgdevid;
					page_key.i_no = i_no;
					page_key.pgindex = pgindex;
				}
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
		for (int i = 0; i < 16; i++) {
			s.append((char)b.get());
		}
		record.procname = s.toString();
		s = new StringBuilder(32);
		for (int i = 0; i < 32; i++) {
			s.append((char)b.get());
		}
		record.devname = s.toString();
		record.make_keys();
		return record;
	}
	
	private static Record RecordGetter(BufferedInputStream in_stream) throws Exception
	{
		byte[] buf = new byte[100];
		int res = in_stream.read(buf);
		switch (res) {
			case 100:
				return RecordReader(buf);
			case 0:
				return null;
		}
		throw new Exception("Bad read size");
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
	
	private static Map<BlockKey,LinkedList<Record>> block_map = new ConcurrentSkipListMap<>(block_Comparator);
	private static Map<PageKey,LinkedList<Record>> page_map = new ConcurrentSkipListMap<>(page_Comparator);

	private static Map<BlockKey,PageKey> block_to_page = new TreeMap<>(block_Comparator);
	private static Map<PageKey,LinkedList<BlockKey>> page_to_block = new TreeMap<>(page_Comparator);
	
	/**
	 * @param args
	 */
	public static void main(String[] args) {
		System.out.println("Hello world\n");
		File inf = new File("/proc/bp/kbpd0");
		BufferedInputStream inb = null;
		Record r;
		
		try {
			inb = new BufferedInputStream(new FileInputStream(inf), 8000);
		} catch (FileNotFoundException e) {
			System.out.println("Cant open file " + e.getMessage() + "\n");
			System.exit(1);
		}
		while (true) {
			try {
				r = RecordGetter(inb);
				if (r != null) {
					System.out.println("Record time:" + r.jiffies + " i_no:" + r.i_no + " block:" + r.block + " devid:" + r.devid + " pgdevid:"	+ r.pgdevid + " pgindex:" + r.pgindex + " size:" + r.size + " pid:" + r.pid + "tgid:" + r.tgid + " reason:" + r.reason + " dev:" + r.devname + " proc:" + r.procname + "\n");
					System.out.println("Keys " + r.block_key + " : " + r.page_key + "\n");
					if (r.block_key != null)System.out.println("Block key " + r.block_key.devid + ":" + r.block_key.block + ":" + r.block_key.size + "\n");
					if (r.page_key != null)System.out.println("Page key " + r.page_key.devid + ":" + r.page_key.i_no + ":" + r.page_key.pgindex + "\n");
					if (r.block_key != null) {
						LinkedList<Record> l = block_map.get(r.block_key);
						if (l == null) {
							l = new LinkedList<>();
							l.add(r);
							block_map.put(r.block_key, l);
						} else l.add(r);
					}
					if (r.page_key != null) {
						LinkedList<Record> l = page_map.get(r.page_key);
						if (l == null) {
							l = new LinkedList<>();
							l.add(r);
							page_map.put(r.page_key, l);
						} else l.add(r);
					}
					if ((r.block_key != null) && (r.page_key != null)) {
						PageKey t = block_to_page.get(r.block_key);
						if ((t != null) && (page_Comparator.compare(t, r.page_key) != 0)) {
							System.out.println("Block " + r.block_key.devid + ":" + r.block_key.block + ":" + r.block_key.size + " was mapped to " + t.devid + ":" + t.i_no + ":" + t.pgindex + " and is now mapped to " + r.page_key.devid + ":" + r.page_key.i_no + ":" + r.page_key.pgindex + "\n");
						}
						block_to_page.put(r.block_key, r.page_key);
						LinkedList<BlockKey> l = page_to_block.get(r.page_key);
						if (l == null) {
							l = new LinkedList<>();
							l.add(r.block_key);
							page_to_block.put(r.page_key, l);
						} else {
							int found = 0;
							for (BlockKey k:l) {
								if (block_Comparator.compare(k, r.block_key)==0) {
									found = 1;
									break;
								}
							}
							if (found == 0)
								l.add(r.block_key);
						}
					}
				}
			} catch (Exception e) {
				System.out.println("Cant read record " + e.getMessage() + "\n");
				System.exit(1);
			}
		}
	}
}
