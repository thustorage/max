#pragma D option dynvarsize=128m

/*
syscall::*:entry
/pid == $1/
{
	self->ts = timestamp;
}
*/

syscall::*:entry
{
	self->ts = timestamp;
}

syscall::*:return
/self->ts/
{
        @avg[probefunc] = avg(timestamp - self->ts);
	@sum[probefunc] = sum(timestamp - self->ts);
        @count[probefunc] = count();
/*
        @time[probefunc] = quantize(timestamp - self->ts);
*/
        self->ts = 0;
}

sched:::off-cpu
/self->ts/
{
	self->off_ts = timestamp;
}

sched:::on-cpu
/self->off_ts/
{
	self->ts += timestamp - self->off_ts;
}

END
{
	printf("---avg---");
	printa(@avg);
	printf("---sum---");
	printa(@sum);
	printf("---count---");
	printa(@count);
}
