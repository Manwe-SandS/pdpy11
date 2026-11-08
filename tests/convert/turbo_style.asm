%1 = cnt
	.la 1000
	mov #5,cnt
	.word 1|2^3, 4<<2*3, 5+2<<1, 1<<2+1
	.end
