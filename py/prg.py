#!/usr/bin/python3

import sys,serial,time,crcmod

#def atcmd(cmnd, resp, to):
#  print(cmnd)
#  return 'OK'

def atcmd(cmnd, resp, to = 0.5):
  if ser.timeout != to:
    ser.timeout = to
  ser.flushInput()
  ser.write((cmnd + '\n').encode('ascii'))
  r = ser.readline().decode('ascii').rstrip();
  if len(resp) > 0 and r.find(resp) == -1:
    if r == '': r = '(none)'
    raise RuntimeError('Error! expected ' + resp + '\ncmnd was: ' + cmnd + '\nresp was: ' + r + '\n')
  return r

if len(sys.argv) < 3:
  print('usage: prg.py serial_if filename')
  exit(1)

f = open(sys.argv[2], 'rb')
b = f.read()
f.close()
print('text size:',len(b))
xmodem_crc_func = crcmod.mkCrcFun(0x11021, rev=False, initCrc=0x0000, xorOut=0x0000)
bcrc = xmodem_crc_func(b)

ser = serial.Serial(sys.argv[1], 4800)
try:
  retries = 5
  while retries:
    try:
      retries -= 1
      atcmd('AT+BUFRDDISP=0', 'OK')
      break
    except:
      pass
  if retries == 0:
    print('avr isp bub not responding')
    exit(1)

  addr = 0

  while addr < len(b):
    nb = min(64, len(b)-addr)
    s = b[addr:addr+nb].hex()
    print(addr,'/',len(b))
    atcmd('AT+BUFWR={}'.format(s), 'OK')
    atcmd('AT+EE24WR={:06x}'.format(addr), 'OK')
    atcmd('AT+EE24RD={:06x},{}'.format(addr,nb), 'OK')
    atcmd('AT+BUFCMP', 'OK')
    addr += nb

  dcrc = atcmd('AT+EE24CRC={}'.format(len(b)), '', 20)
  print('Device CRC:',dcrc)
  print('File CRC  :',hex(bcrc)[2:])
  print('Done.')
except Exception as e:
  print(str(e))
finally:
  ser.close()
