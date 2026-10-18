import sys,os,tty,termios
tty.setraw(0)
b=os.read(0,1)
os.write(1,b'GOT:'+b+b'\n')
