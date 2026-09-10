// Shared by the installer and its deterministic host checks. No device I/O.
(function (root) {
  const version = value => {
    if (typeof value !== 'string' || !/^(0|[1-9]\d*)\.(0|[1-9]\d*)\.(0|[1-9]\d*)$/.test(value)) throw Error('version');
    const parts = value.split('.').map(Number);
    if (parts.some(n => n > 0xffffffff)) throw Error('version');
    return parts;
  };
  const compare = (a,b) => {const x=version(a),y=version(b);return x[0]-y[0]||x[1]-y[1]||x[2]-y[2]};
  const digest = value => typeof value==='string' && /^[a-f0-9]{64}$/.test(value);
  const https = value => {try {const u=new URL(value);return value.length<=512 && u.protocol==='https:' && !/[\s\x00-\x1f]/.test(value)}catch{return false}};
  function validate(catalog,target) {
    if (catalog?.schema!==2 || catalog.target!==target || !Array.isArray(catalog.factory_releases) || catalog.factory_releases.length>32) throw Error('catalog');
    const latest=catalog.latest;version(latest?.version);
    if (!digest(latest.layout)||!digest(latest.sha256)||!https(latest.url)) throw Error('catalog');
    let previous=null;
    for (const release of catalog.factory_releases) {
      version(release.version);
      if ((previous&&compare(release.version,previous.version)<=0)||compare(release.version,latest.version)>0||!digest(release.layout)||!https(release.install_manifest)) throw Error('catalog');
      previous=release;
    }
    if(previous&&previous.layout!==latest.layout)throw Error('catalog');
    return catalog;
  }
  function select(catalog,target,current,layout) {
    validate(catalog,target);version(current);
    const latest=catalog.latest;
    if(compare(current,latest.version)>0)return {release:latest,factory:false};
    let required=null;
    for (const boundary of catalog.factory_releases) if(compare(boundary.version,current)>0)required=boundary;
    if(!required&&latest.layout!==layout)required=catalog.factory_releases.at(-1);
    if(required)return {release:required,factory:true};
    if(latest.layout!==layout)throw Error('catalog');
    return {release:latest,factory:false};
  }
  const text=(data,start,length)=>new TextDecoder().decode(data.subarray(start,start+length)).split('\0')[0];
  function identity(data) {
    if(data.length<288 || data[0]!==0xe9 || data[12]!==9 || data[13]!==0 || new DataView(data.buffer,data.byteOffset).getUint32(32,true)!==0xabcd5432 || text(data,80,32)!=='printdeck')throw Error('identity');
    const current=text(data,48,32);version(current);
    if(data.length<388 || text(data,288,16)!=='PrintDeck OTA 1')return {version:current,legacy:true};
    const target=text(data,304,16),layout=text(data,320,65);
    if(!digest(layout)||!['amoled_1_75','lcd_1_54','knomi2'].includes(target))throw Error('identity');
    return {version:current,target,layout,legacy:false};
  }
  function partitions(data) {
    if(data.length!==3072)throw Error('partitions');
    const view=new DataView(data.buffer,data.byteOffset,data.byteLength), entries=[];
    for(let i=0;i+32<=data.length;i+=32) {
      if(view.getUint16(i,true)!==0x50aa)break;
      const entry={type:data[i+2],subtype:data[i+3],offset:view.getUint32(i+4,true),size:view.getUint32(i+8,true),name:text(data,i+12,16)};
      if(entry.size===0||entry.offset+entry.size>0x1000000||entries.some(p=>p.offset<entry.offset+entry.size&&entry.offset<p.offset+p.size))throw Error('partitions');
      entries.push(entry);
    }
    const ota=entries.filter(e=>e.type===0&&e.subtype>=16&&e.subtype<32).sort((a,b)=>a.subtype-b.subtype);
    const selector=entries.find(e=>e.type===1&&e.subtype===0);
    if(ota.length!==2||ota[0].subtype!==16||ota[1].subtype!==17||selector?.size!==8192)throw Error('partitions');
    return {ota,selector};
  }
  function bootSlot(selector) {
    if(selector.length!==8192)throw Error('selector');
    if(selector.every(b=>b===255))return 0;
    const view=new DataView(selector.buffer,selector.byteOffset,selector.byteLength), valid=[];
    for (const offset of [0,4096]) {
      const seq=view.getUint32(offset,true),state=view.getUint32(offset+24,true);
      // ESP-IDF esp_rom_crc32_le(UINT32_MAX, ota_seq, 4).
      let crc=0;
      for(let i=0;i<4;i++){crc^=selector[offset+i];for(let b=0;b<8;b++)crc=(crc>>>1)^((crc&1)?0xedb88320:0)}
      crc=(~crc)>>>0;
      if(seq!==0xffffffff&&seq>0&&state!==3&&state!==4&&view.getUint32(offset+28,true)===crc)valid.push(seq);
    }
    if(!valid.length)throw Error('selector');
    return (Math.max(...valid)-1)%2;
  }
  async function validateImage(data, expectedHash) {
    if(data.length<388 || data[0]!==0xe9 || data[1]<1 || data[1]>16 || data[23]!==1)throw Error('image');
    const view=new DataView(data.buffer,data.byteOffset,data.byteLength);
    let cursor=24,checksum=0xef;
    for(let segment=0;segment<data[1];segment++) {
      if(cursor+8>data.length)throw Error('image');
      const size=view.getUint32(cursor+4,true);cursor+=8;
      if(size>data.length-cursor)throw Error('image');
      for(let i=cursor;i<cursor+size;i++)checksum^=data[i];
      cursor+=size;
    }
    const end=Math.ceil((cursor+1)/16)*16;
    if(end+32!==data.length||data[end-1]!==checksum)throw Error('image');
    const digest=new Uint8Array(await crypto.subtle.digest('SHA-256',data.subarray(0,end)));
    if(!digest.every((byte,i)=>byte===data[end+i]))throw Error('image');
    if(expectedHash) {
      const hash=[...new Uint8Array(await crypto.subtle.digest('SHA-256',data))].map(b=>b.toString(16).padStart(2,'0')).join('');
      if(hash!==expectedHash)throw Error('image');
    }
  }
  root.PrintDeckUpdatePolicy={validate,select,compare,identity,partitions,bootSlot,validateImage};
})(globalThis);
