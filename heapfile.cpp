#include "heapfile.h"
#include "error.h"
extern BufMgr* bufMgr; 

#define CHKSTAT2(c) { if(c != OK) { \
                          returnStatus =  c; \
                        } \
                      }

// routine to create a heapfile
const Status createHeapFile(const string fileName)
{
  File* 		file;
  Status 		status;
  FileHdrPage*	hdrPage;
  int			hdrPageNo;
  //  int			newPageNo;
  //  Page*		newPage;

  // try to open the file. This should return an error
  status = db.openFile(fileName, file);
  if (status != OK)
    {
      // file doesn't exist. First create it and allocate
      // an empty header page and data page.
      status =  db.createFile(fileName);
      CHKSTAT(status); //BADFILE, FILEEXISTS, UNIXERR
      status = db.openFile(fileName, file);
      CHKSTAT(status); //BADFILE, UNIXERR
      // initialize fileHdrPage
      Page* headerPage;
      status = bufMgr->allocPage(file, hdrPageNo, headerPage);
      CHKSTAT(status); //UNIXERR, HASHTBLERR, BUFFEREXCEEDED
      hdrPage = (FileHdrPage*) headerPage;
      // initialize header page struct
      strcpy(hdrPage->fileName, fileName.c_str());
      hdrPage->pageCnt = 1;
      hdrPage->recCnt = 0;
      // initializr first data page
      Page* firstDataPage;
      status = bufMgr->allocPage(file, hdrPage->firstPage, firstDataPage);
      CHKSTAT(status); //UNIXERR, HASHTBLERR, BUFFEREXCEEDED
      firstDataPage->init(hdrPage->firstPage);
      hdrPage->lastPage = hdrPage->firstPage;
      // unpin, dirty
      status = bufMgr->unPinPage(file, hdrPage->firstPage, true);
      CHKSTAT(status); //HASHNOTFOUND, PAGENOTPINNED
      status = bufMgr->unPinPage(file, hdrPageNo, true);
      CHKSTAT(status); //HASHNOTFOUND, PAGENOTPINNED
      return OK;
    }
  return (FILEEXISTS);
}

// routine to destroy a heapfile
const Status destroyHeapFile(const string fileName)
{
	return (db.destroyFile (fileName));
}

// constructor opens the underlying file
HeapFile::HeapFile(const string & fileName, Status& returnStatus)
{
  Status 	status;
  Page*	pagePtr;

  cout << "opening file " << fileName << endl;

  // open the file and read in the header page and the first data page
  if ((status = db.openFile(fileName, filePtr)) == OK)
    {
      // read and pin header page
      status = filePtr->getFirstPage(headerPageNo);
      CHKSTAT2(status); // UNIXERR
      status = bufMgr->readPage(filePtr, headerPageNo, pagePtr);
      headerPage = (FileHdrPage*) pagePtr;
      CHKSTAT2(status); // UNIXERR, BUFFEREXCEEDED, HASHTBLERR
      hdrDirtyFlag = false;
      // read and pin first data page
      curPageNo = headerPage->firstPage;
      status = bufMgr->readPage(filePtr, curPageNo, curPage);
      CHKSTAT2(status); // UNIXERR, BUFFEREXCEEDED, HASHTBLERR
      curDirtyFlag = false;
      curRec = NULLRID;
      returnStatus = OK;
      return;
    }
  else
    {
      cerr << "open of heap file failed\n";
      returnStatus = status;
      return;
    }
}

// the destructor closes the file
HeapFile::~HeapFile()
{
    Status status;
    cout << "invoking heapfile destructor on file " << headerPage->fileName << endl;

    // see if there is a pinned data page. If so, unpin it 
    if (curPage != NULL)
    {
    	status = bufMgr->unPinPage(filePtr, curPageNo, curDirtyFlag);
		curPage = NULL;
		curPageNo = 0;
		curDirtyFlag = false;
		if (status != OK) cerr << "error in unpin of date page\n";
    }
	
	 // unpin the header page
    status = bufMgr->unPinPage(filePtr, headerPageNo, hdrDirtyFlag);
    if (status != OK) cerr << "error in unpin of header page\n";
	
	// status = bufMgr->flushFile(filePtr);  // make sure all pages of the file are flushed to disk
	// if (status != OK) cerr << "error in flushFile call\n";
	// before close the file
	status = db.closeFile(filePtr);
    if (status != OK)
    {
		cerr << "error in closefile call\n";
		Error e;
		e.print (status);
    }
}

// Return number of records in heap file

const int HeapFile::getRecCnt() const
{
  return headerPage->recCnt;
}

// retrieve an arbitrary record from a file.
// if record is not on the currently pinned page, the current page
// is unpinned and the required page is read into the buffer pool
// and pinned.  returns a pointer to the record via the rec parameter

const Status HeapFile::getRecord(const RID &  rid, Record & rec)
{
  Status status;

  // cout<< "getRecord. record (" << rid.pageNo << "." << rid.slotNo << ")" << endl;
  if(rid.pageNo == curPageNo){
    // this page is currently pinned
    status = curPage->getRecord(rid, rec);
    return status; // INVALIDSLOTNO
  } else {
    // this page is not pinned
    status = bufMgr->unPinPage(filePtr, curPageNo, curDirtyFlag);
    CHKSTAT(status); // HASHNOTFOUND, PAGENOTPINNED
    status = bufMgr->readPage(filePtr, rid.pageNo, curPage);
    CHKSTAT(status); // UNIXERR, HASHTBLERR, BUFFEREXCEEDED
    curPageNo = rid.pageNo;
    status = curPage->getRecord(rid, rec);
    return status;
  }
}

HeapFileScan::HeapFileScan(const string & name,
			   Status & status) : HeapFile(name, status)
{
    filter = NULL;
}

const Status HeapFileScan::startScan(const int offset_,
				     const int length_,
				     const Datatype type_, 
				     const char* filter_,
				     const Operator op_)
{
    if (!filter_) {                        // no filtering requested
        filter = NULL;
        return OK;
    }
    
    if ((offset_ < 0 || length_ < 1) ||
        (type_ != STRING && type_ != INTEGER && type_ != FLOAT) ||
        ((type_ == INTEGER && length_ != sizeof(int))
         || (type_ == FLOAT && length_ != sizeof(float))) ||
        (op_ != LT && op_ != LTE && op_ != EQ && op_ != GTE && op_ != GT && op_ != NE))
    {
        return BADSCANPARM;
    }

    offset = offset_;
    length = length_;
    type = type_;
    filter = filter_;
    op = op_;

    return OK;
}


const Status HeapFileScan::endScan()
{
    Status status;
    // generally must unpin last page of the scan
    if (curPage != NULL)
    {
        status = bufMgr->unPinPage(filePtr, curPageNo, curDirtyFlag);
        curPage = NULL;
        curPageNo = 0;
		curDirtyFlag = false;
        return status;
    }
    return OK;
}

HeapFileScan::~HeapFileScan()
{
    endScan();
}

const Status HeapFileScan::markScan()
{
    // make a snapshot of the state of the scan
    markedPageNo = curPageNo;
    markedRec = curRec;
    return OK;
}

const Status HeapFileScan::resetScan()
{
    Status status;
    if (markedPageNo != curPageNo) 
    {
		if (curPage != NULL)
		{
			status = bufMgr->unPinPage(filePtr, curPageNo, curDirtyFlag);
			if (status != OK) return status;
		}
		// restore curPageNo and curRec values
		curPageNo = markedPageNo;
		curRec = markedRec;
		// then read the page
		status = bufMgr->readPage(filePtr, curPageNo, curPage);
		if (status != OK) return status;
		curDirtyFlag = false; // it will be clean
    }
    else curRec = markedRec;
    return OK;
}


const Status HeapFileScan::scanNext(RID& outRid)
{
  Status 	status = OK;
  RID		nextRid;
  RID		tmpRid;// it is curr rid; 
  int 	nextPageNo;
  Record      rec;
    
  status = this->markScan();
  tmpRid = markedRec;
  while(status != FILEEOF){
    status = curPage->nextRecord(tmpRid,nextRid);
    // if we reach the last record of a page; 
    if (ENDOFPAGE == status){
      // if the current page is not the end of file,
      // we continue to check the next page of file;
      // otherwise we finish the scanning. 
      if(headerPage->lastPage == curPageNo){
	status = FILEEOF;// end of file
	break;
      } else {
	status = curPage->getNextPage(nextPageNo);//get nextPage;
	if(status != OK) return status; 
	status = bufMgr->unPinPage(filePtr, curPageNo, curDirtyFlag);
	if(status != OK) return status;
	curPageNo = nextPageNo;
	curDirtyFlag = false;
	status = bufMgr->readPage(filePtr, curPageNo, curPage);
	if(status != OK) return status;
      }
    } else{
      // if it is not the last page;    
      status = curPage->getRecord(nextRid, rec);
      //check if this record is match;
      if(this->matchRec(rec)){
	outRid = nextRid;
	curRec = nextRid;//markScan..
	break;
      }
      else tmpRid = nextRid;
    }
  }
  return status;
}


// returns pointer to the current record.  page is left pinned
// and the scan logic is required to unpin the page 

const Status HeapFileScan::getRecord(Record & rec)
{
    return curPage->getRecord(curRec, rec);
}

// delete record from file. 
const Status HeapFileScan::deleteRecord()
{
    Status status;

    // delete the "current" record from the page
    status = curPage->deleteRecord(curRec);
    curDirtyFlag = true;

    // reduce count of number of records in the file
    headerPage->recCnt--;
    hdrDirtyFlag = true; 
    return status;
}


// mark current page of scan dirty
const Status HeapFileScan::markDirty()
{
    curDirtyFlag = true;
    return OK;
}

const bool HeapFileScan::matchRec(const Record & rec) const
{
    // no filtering requested
    if (!filter) return true;

    // see if offset + length is beyond end of record
    // maybe this should be an error???
    if ((offset + length -1 ) >= rec.length)
	return false;

    float diff = 0;                       // < 0 if attr < fltr
    switch(type) {

    case INTEGER:
        int iattr, ifltr;                 // word-alignment problem possible
        memcpy(&iattr,
               (char *)rec.data + offset,
               length);
        memcpy(&ifltr,
               filter,
               length);
        diff = iattr - ifltr;
        break;

    case FLOAT:
        float fattr, ffltr;               // word-alignment problem possible
        memcpy(&fattr,
               (char *)rec.data + offset,
               length);
        memcpy(&ffltr,
               filter,
               length);
        diff = fattr - ffltr;
        break;

    case STRING:
        diff = strncmp((char *)rec.data + offset,
                       filter,
                       length);
        break;
    }

    switch(op) {
    case LT:  if (diff < 0.0) return true; break;
    case LTE: if (diff <= 0.0) return true; break;
    case EQ:  if (diff == 0.0) return true; break;
    case GTE: if (diff >= 0.0) return true; break;
    case GT:  if (diff > 0.0) return true; break;
    case NE:  if (diff != 0.0) return true; break;
    }

    return false;
}

InsertFileScan::InsertFileScan(const string & name,
                               Status & status) : HeapFile(name, status)
{
  //Do nothing. Heapfile constructor will read the header page and the first
  // data page of the file into the buffer pool
}

InsertFileScan::~InsertFileScan()
{
    Status status;
    // unpin last page of the scan
    if (curPage != NULL)
    {
        status = bufMgr->unPinPage(filePtr, curPageNo, true);
        curPage = NULL;
        curPageNo = 0;
        if (status != OK) cerr << "error in unpin of data page\n";
    }
}

// Insert a record into the file
const Status InsertFileScan::insertRecord(const Record & rec, RID& outRid)
{
  Page*	newPage;
  int		newPageNo;
  Status	status;//, unpinstatus;
  RID		rid;

  // check for very large records
  if ((unsigned int) rec.length > PAGESIZE-DPFIXED)
    {
      // will never fit on a page, so don't even bother looking
      return INVALIDRECLEN;
    }
    
  // check if the curr page is not the last page, we need to make
  // currPage pointing to last page, then insert. 
  if(headerPage->lastPage != curPageNo){
      status = bufMgr->unPinPage(filePtr, curPageNo, curDirtyFlag);
      CHKSTAT(status);
      status = bufMgr->readPage(filePtr, headerPage->lastPage, curPage);
      CHKSTAT(status);
      curDirtyFlag = false;
      curPageNo = headerPage->lastPage;
  }
    
  //insert:
  status = curPage->insertRecord(rec, rid);
  // check if this page is full;
  if(status == NOSPACE){
    status = bufMgr->allocPage(filePtr, newPageNo, newPage);
    CHKSTAT(status);
    newPage->init(newPageNo);
    status = curPage->setNextPage(newPageNo);
    CHKSTAT(status);
    // unpin the currPage;
    status = bufMgr->unPinPage(filePtr, curPageNo, curDirtyFlag);
    CHKSTAT(status);
	
    // update the headerPage and insert the record;
    headerPage->lastPage = newPageNo;
    headerPage->pageCnt++;
	
    curPage = newPage;
    curPageNo = newPageNo;
    curDirtyFlag = false;
	
    status = curPage->insertRecord(rec,rid);
    CHKSTAT(status);
  }

  if(status == OK){
    // update curRec as well
    curRec = rid;
    outRid = rid;
    curDirtyFlag = true;
    headerPage->recCnt++;// increase the recCnt. 
  }
  return status; 
}


