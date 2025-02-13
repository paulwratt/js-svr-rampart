//first line
rampart.globalize(rampart.utils);

chdir(process.scriptPath);

function testFeature(name,test)
{
    var error=false;
    if (typeof test =='function'){
        try {
            test=test();
        } catch(e) {
            error=e;
            test=false;
        }
    }
    printf("testing utils - %-52s - ", name);
    if(test)
        printf("passed\n")
    else
    {
        printf(">>>>> FAILED <<<<<\n");
        if(error) console.log(error);
        process.exit(1);
    }
    if(error) console.log(error);
}


printf("testing utils - %-52s - ", "printf");
printf("passed\n");

fprintf(stdout,"testing utils - %-52s - ", "fprintf(stdout,...)");
fprintf(stdout,"passed\n");

fprintf(stderr,"testing utils - %-52s - ", "fprintf(stderr,...)");
fprintf(stderr,"passed\n");


testFeature("fopen/fseek/fprint/fread/fwrite/rewind",function(){

    var fh=fopen("test.txt","w+");

    fh.fprintf("abcdef");

    fh.fprintf("ghijkl");

    fh.rewind();
    fh.fprintf("123");

    fh.fseek(-3,"SEEK_END");
    fh.fprintf("456");

    fh.rewind();
    var buf=fh.fread(1000);

    var out="789abcdefghijklmnopqrstuvwxyz";
    fh.fwrite(out,3);

    fh.rewind();
//    var res1=bufferToString(fread(fh,1000));
    var res1=fh.fread(1000,true);
    rmFile("test.txt");
    return res1=="123defghi456789";
});


var str=readFile(process.script,-220,-20,true);

testFeature("readFile/string",
    str.length==200
);

var buf=readFile(process.script,{offset:-220,length:-20,retString:false});

testFeature("readFile/buf - bufferToString",function(){
    return buf.length==200 && bufferToString(buf)==str;
});

testFeature("readLine/trim",function(){
    var rl=readLine(process.script);
    var i=0;
    var line;
    var firstline;
    var lastline;
    while ( (line=rl.next()) ) {
        if(i==0)
            firstline=trim(line);
        i++;
        lastline=line;
    }
    return firstline=="//first line" && trim(lastline)=="//lastline";
});

testFeature("stat",function(){
    var st=stat("/dev/null");
    return st.isCharacterDevice;
});



testFeature("execRaw/timeout",function(){
    var ret=execRaw({
        path:"/bin/sleep",
        args:["sleep","10"],
        timeout:200
    });
    return ret.timedOut;
});

testFeature("exec/env/stdin",function(){
    var ret=exec("head", "-n", "1", process.script);
    var ret2=exec('env',{env:{myvar:"myval"}});
    var ret3=exec('cat',{stdin:"hello"});
    return (
        trim(ret.stdout) == "//first line" &&
        trim(ret2.stdout)== "myvar=myval" &&
        ret3.stdout == "hello"
    );

});

testFeature("shell",function(){
    var ret=shell("tail -n 1 "+ process.script, {timeout:2000} );
    return trim(ret.stdout)=="//lastline";
});


testFeature("exec/bkgrnd/kill",function(){
    var ret=exec("sleep", "30", {background:true});
    return kill(ret.pid);
});

testFeature("mkdir/rmdir/stat",function(){
    mkdir("t1/t2",0777);
    var stat1=stat("t1/t2");
    rmdir("t1/t2",true);
    var mode1=sprintf("%o",stat1.mode & 0777);
    var stat2=true;
    stat2=stat("t1");
    return (!stat2 && mode1=="777");
});

testFeature("readdir",function(){
    var gotdot=false;
    var gotthis=false;
    readdir(process.scriptPath,true).forEach(function(d){
        if(d==".") gotdot=true;
        if(d==process.scriptName) gotthis=true
    });
    return gotthis && gotdot;
});

testFeature("copy/delete",function(){
    copyFile(process.script,"test1.js",true);
    var stat1=stat("test1.js");
    var diff=shell("diff "+ process.script +" test1.js");
    rmFile("test1.js");
    var stat2=stat("test1.js");
    return stat1.mode && !stat2 && diff.stdout == "";
});

testFeature("symlink/delete/lstat",function(){
    symlink(process.script,"test1.js");
    var islink=lstat("test1.js").isSymbolicLink;
    var diff=shell("diff "+ process.script +" test1.js");
    rmFile("test1.js");
    var stat2=stat("test1.js");
    return islink && !stat2 && diff.stdout == "";
});

testFeature("hard link/delete",function(){
    fprintf("myfile.txt","a message to nobody");
    link({
        src:"myfile.txt",
        target:"test1.txt"
    });
    var stat1=stat("test1.txt");
    var test=shell("if [ test1.txt -ef myfile.txt ]; then echo yes; fi");
    rmFile("test1.txt");
    var stat2=stat("test1.txt");
    return stat1.mode && !stat2 && test.stdout == "yes\n";
});

testFeature("copy over hard/sym link throw",function(){
    link("myfile.txt", "hardlink");
    symlink("hardlink", "symlink");
    var ret=false;
    try{
        copyFile("myfile.txt", "symlink");
    } catch(e) {
        //console.log(e);
        ret=true;
    }
    rmFile("hardlink");
    rmFile("symlink");
    rmFile("myfile.txt");
    return ret;
});

testFeature("touch/rename",function(){
    touch("myfile");
    var stat1=stat("myfile");
    rename("myfile","/tmp/myfile"); //copies if different mounted fs
    var stat2=stat("/tmp/myfile");
    rmFile("/tmp/myfile");
    return stat1 && stat2;
});

testFeature("reference touch",function(){
    touch({
        path:"myfile",
        reference:process.script
    });
    var stat1=stat("myfile");
    var stat2=stat(process.script);
    rmFile("myfile");

    return stat1.atime.getSeconds() == stat2.atime.getSeconds() && stat1.mtime.getSeconds() == stat2.mtime.getSeconds();
});

var wai=trim(shell("whoami").stdout);
if (wai=="root")
{
    testFeature("chown", function(){
        touch("myfile");
        chown({
            path:"myfile",
            group:101,
            user: 100
        });
        
        var stat1=stat("myfile");
        rmFile("myfile");
        return stat1.uid == 100 && stat1.gid == 101;
    });

} else
    printf("skipping chown test, must be done as root\n");


//lastline