Snake-game can be run on embedded board.
In my example, i run it on my Raspberry pi 4b
Step 1:
	Download my git folder to your computer, use command "make".

Step 2:
	Copy .ko file to your Raspberry Pi 4b

Step 3:
	Using command: "sudo insmod ssd1306.ko speed=x"

Note:
	X is your speed game, if you just used "sudo insmod ssd1306.ko", default speed is 4.

__END__
