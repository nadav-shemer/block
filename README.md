block
=====
<pre>
block and page-level tracing of events
Kernel module creates a record for each event, written to a per-CPU FIFO
Userspace can read from the FIFO through /proc/bp/kbpd0..N

Events traced:
Page events: Get / Dirty / Accessed / Deactivated / Activated / Evicted
Buffer events: Read / Get / Dirty / Evicted
When a page is traced while locked (get, dirty, evicted), its buffers are traced as well

Currently record size and usage is not well optimized
</pre>

Contents
====
<pre>
patch - kernel patch file
kernel/ - The patched files, expanded (some unpatched files present as well)
Source is ubuntu's 3.5.0-19-generic
readtrace/ - Userspace reader in C
start/ - Userspace reader in Java
	 Creates a mapping between blocks and pages (using page events that carry block data)
</pre>
