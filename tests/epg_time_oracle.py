"""Independent test-only TS 102 371 time decoder; no production parser calls.
The supplied legacy JSON used the wrong hour bit and is retained unchanged.
"""
from datetime import datetime,timedelta,timezone

def time_point(raw):
    word=int.from_bytes(raw[:4],'big')
    long_form=bool(word & (1<<11));present=bool(word & (1<<12))
    size=6 if long_form else 4
    assert len(raw)==size+int(present)
    mjd=(word>>14)&0x1ffff
    hour=(word>>6)&31;minute=word&63
    second=(raw[4]>>2) if long_form else 0
    assert 40587<=mjd<=99999 and hour<24 and minute<60 and second<60
    offset=(raw[size]&31)*30*(-1 if raw[size]&32 else 1) if present else 0
    assert abs(offset)<=840
    value=datetime(1858,11,17,tzinfo=timezone.utc)+timedelta(days=mjd,hours=hour,minutes=minute,seconds=second)
    return int(value.timestamp()),offset,present

def children(data):
    pos=0
    while pos<len(data):
        tag,length=data[pos:pos+2];pos+=2
        if length>=254:
            count=2 if length==254 else 3
            length=int.from_bytes(data[pos:pos+count],'big');pos+=count
        assert pos+length<=len(data)
        yield tag,data[pos:pos+length]
        pos+=length

def programme_times(data):
    result={}
    def visit(data):
        for tag,body in children(data):
            if tag==0x1c:
                fields=list(children(body));pid=next(int.from_bytes(b,'big') for t,b in fields if t==0x81)
                def find_time(data):
                    for t,b in children(data):
                        if t==0x2c:
                            for a,v in children(b):
                                if a==0x80:return v
                        if t in (0x19,0x2d):
                            found=find_time(b)
                            if found is not None:return found
                raw=find_time(body)
                assert raw is not None,pid
                result[pid]=time_point(raw)
            elif tag in (2,0x21):visit(body)
    visit(data)
    return result
