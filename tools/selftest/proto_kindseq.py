"""验证：独立 Python 实现的【kind 序列】能否在 490 个文件上匹配 C++ 实现。
若能，就能把它加进 check_lexer.py，堵上"最长匹配"盲点。"""
import re, glob, sys

KEYWORDS = {'const':'kw_const','int':'kw_int','float':'kw_float','void':'kw_void',
 'if':'kw_if','else':'kw_else','while':'kw_while','break':'kw_break',
 'continue':'kw_continue','return':'kw_return'}

TOKEN_RE = re.compile(r'''
      [A-Za-z_][A-Za-z_0-9]*
    | 0[xX](?:[0-9a-fA-F]+(?:\.[0-9a-fA-F]*)?|\.[0-9a-fA-F]+)(?:[pP][+-]?[0-9]+)?
    | (?:[0-9]+\.[0-9]*|\.[0-9]+|[0-9]+)(?:[eE][+-]?[0-9]+)?
    | [0-9]+
    | <=|>=|==|!=|&&|\|\|
    | [-+*/%<>=!(){}\[\],;]
''', re.X)
SIMPLE = {'+':'plus','-':'minus','*':'star','/':'slash','%':'percent',
 '<':'less','>':'greater','=':'assign','!':'not',
 '(':'lparen',')':'rparen','{':'lbrace','}':'rbrace',
 '[':'lbracket',']':'rbracket',',':'comma',';':'semicolon'}
DOUBLE = {'<=':'lesseq','>=':'greatereq','==':'eqeq','!=':'noteq',
 '&&':'ampamp','||':'pipepipe'}

def kinds(text):
    """独立实现的 kind 序列（不读 C++ 代码）。"""
    out=[]; i=0; n=len(text)
    while i<n:
        c=text[i]
        if c in ' \t\r\n\v\f': i+=1; continue
        if c=='/' and i+1<n and text[i+1]=='/':
            while i<n and text[i]!='\n': i+=1
            continue
        if c=='/' and i+1<n and text[i+1]=='*':
            i+=2
            while i+1<n and not(text[i]=='*' and text[i+1]=='/'): i+=1
            i=min(i+2,n); continue
        m=TOKEN_RE.match(text,i)
        if not m: out.append('invalid'); i+=1; continue
        tok=m.group(0)
        # 分类顺序很关键：先字面量，再关键字/标识符
        if tok in DOUBLE: out.append(DOUBLE[tok])
        elif tok in SIMPLE: out.append(SIMPLE[tok])
        elif tok[:2].lower() == '0x':
            # 十六进制：有小数点或 p 指数才是浮点（hex 里的 e 不是指数！）
            out.append('floatlit' if ('.' in tok or 'p' in tok or 'P' in tok) else 'intlit')
        elif tok[0].isdigit() or tok[0] == '.':
            # 十进制/八进制：有小数点或 e/E 指数才是浮点
            out.append('floatlit' if ('.' in tok or 'e' in tok or 'E' in tok) else 'intlit')
        elif tok in KEYWORDS: out.append(KEYWORDS[tok])
        else: out.append('ident')
        i=m.end()
    return out

files=sorted(glob.glob('tests/**/*.sy', recursive=True))
skip=ok=bad=0; probs=[]
import subprocess, os
for f in files:
    raw=open(f,'rb').read().decode('utf-8','replace')
    text=raw.replace('\r\n','\n').replace('\r','\n')
    if re.search(r'@|\btensor\b', text): skip+=1; continue
    r=subprocess.run(['compiler/build/compiler',f,'--emit=tokens','-o','/dev/stdout'],
                     capture_output=True)
    if r.returncode!=0: skip+=1; continue
    cpp=[]
    for line in r.stdout.decode('utf-8','replace').split('\n'):
        p=line.split('\t')
        if len(p)==4 and p[2]!='EOF': cpp.append(p[2])
    py=kinds(text)
    if cpp==py: ok+=1
    else:
        bad+=1
        for k in range(max(len(cpp),len(py))):
            a=cpp[k] if k<len(cpp) else '<none>'
            b=py[k] if k<len(py) else '<none>'
            if a!=b: probs.append((f,"第%d个: cpp=%s py=%s"%(k,a,b))); break
print("范围外跳过: %d" % skip)
print("kind 序列比对: 通过=%d 失败=%d" % (ok,bad))
for f,w in probs[:8]: print("  %s\n     %s" % (f,w))
