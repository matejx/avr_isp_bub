#!/usr/bin/python

# avr isp bub hex/bin programmer, Matej Kogovsek (matej@hamradio.si), 16.mar.2017
# adapted from bus gofer prg
# requires pySerial, http://pyserial.sourceforge.net
# and intelhex, http://www.bialix.com/intelhex/

import sys,serial,datetime,intelhex,crc16pure

SER_SPEED = 4800

def atcmd(cmnd, resp, to = 0.5):
  if ser.timeout != to:
    ser.timeout = to
  ser.flushInput()
  ser.write(cmnd + '\n')
  r = ser.readline().rstrip();
  if len(resp) > 0 and r.find(resp) == -1:
    if r == '': r = '(none)'
    raise Exception('Error! expected ' + resp + '\ncmnd was: ' + cmnd + '\nresp was: ' + r + '\n')
  return r

if len(sys.argv) < 3:
  print 'usage: prg.py serial_if filename'
  sys.exit(1)

sif = sys.argv[1]
fn = sys.argv[2]
pgw_size = 64

try:
  h = intelhex.IntelHex()
  if fn[-3:] == 'hex':
    h.loadhex(fn)
  else:
    h.loadbin(fn)
except intelhex.HexReaderError, e:
  print "Bad hex file\n", str(e)
  sys.exit(1)

ser = serial.Serial(sif, SER_SPEED)
try:
  for i in range(5):
    try:
      atcmd('AT+BUFRDDISP=0', 'OK')
      break
    except:
      pass
  if i == 4:
    print 'avr isp bub not responding'
    sys.exit(1)

  hna = h.minaddr() # veeeeeery slow function
  hma = h.maxaddr() # same
  a = hna
  sdt = datetime.datetime.now()
  tb = 0
  crc = 0
  while a < hma:
    s = ''
    adr = hex(a)[2:].zfill(6)
    nb = min(pgw_size, hma - a + 1)
    tb = tb + nb
    for i in range(nb):
      s = s + hex(h[a])[2:].zfill(2)
      a = a + 1
    crc = crc16pure.crc16xmodem(s.decode('hex'), crc)
    try:
      te = datetime.datetime.now() - sdt # time elapsed
      pd = 1.0 * (a-hna) / (hma-hna) # prg done
      tl = datetime.timedelta(seconds = int(((1.0-pd)/pd) * te.total_seconds())) # time left
      print adr,nb,tb,'/',hma-hna+1,'(',int(100.0*pd),'%)',tl,' left'
      atcmd('AT+BUFWR=' + s, 'OK')
      atcmd('AT+EE24WR=' + adr, 'OK')
      atcmd('AT+EE24RD=' + adr + ',' + str(i + 1), 'OK')
      atcmd('AT+BUFCMP', 'OK')
    except Exception, e:
      print e
      break
  print
  if hna == 0:
    print 'Device CRC:',atcmd('AT+EE24CRC=' + str(hma+1), '', 20)
    print 'File CRC  :',hex(crc)[2:]
finally:
  ser.close()

print 'Done.'
