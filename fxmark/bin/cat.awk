BEGIN{
	io_util=0.0
	count=0
}
{
	if ($NF >= 5){
	    io_util+=$NF
	    count++
	}
}
END{
	if (count !=0 ){
		printf("%f",io_util/count)
	}else{
		printf("%f",0)
	}
}
