// Copyright (c) 2014-2017 Coin Sciences Ltd
// MultiChain code distributed under the GPLv3 license, see COPYING file.

#include "protocol/relay.h"
#include "structs/base58.h"
#include "wallet/chunkdb.h"
#include "wallet/chunkcollector.h"
#include "wallet/wallettxs.h"

uint32_t MultichainNextChunkQueryAttempt(uint32_t attempts)
{
    if(attempts <  4)return 0;
    if(attempts <  8)return 60;
    if(attempts < 12)return 3600;
    if(attempts < 16)return 86400;
    if(attempts < 20)return 86400*30;
    if(attempts < 24)return 86400*365;
    return 86400*365*10;
}

typedef struct CRelayResponsePair
{
    int64_t request_id;
    int response_id;
    
    friend bool operator<(const CRelayResponsePair& a, const CRelayResponsePair& b)
    {
        return ((a.request_id < b.request_id) || 
                (a.request_id == b.request_id && a.response_id < b.response_id));
    }
    
} CRelayResponsePair;

typedef struct CRelayRequestPairs
{
    map<int,int> m_Pairs;
} CRelayRequestPairs;

int MultichainProcessChunkResponse(const CRelayResponsePair *response_pair,map <int,int>* request_pairs,mc_ChunkCollector* collector)
{
    mc_RelayRequest *request;
    mc_RelayResponse *response;
    request=pRelayManager->FindRequest(response_pair->request_id);
    
    if(request == NULL)
    {
        return false;
    }
    
    response=&(request->m_Responses[response_pair->response_id]);
    
    unsigned char *ptr;
    unsigned char *ptrEnd;
    unsigned char *ptrStart;
    int shift,count,size;
    int shiftOut,countOut,sizeOut;
    int chunk_err;
    mc_ChunkEntityKey *chunk;
    unsigned char *ptrOut;
    bool result=false;
    mc_ChunkCollectorRow *collect_row;
        
    uint32_t total_size=0;
    ptrStart=&(request->m_Payload[0]);
    
    size=sizeof(mc_ChunkEntityKey);
    shift=0;
    count=0;
    
    ptr=ptrStart;
    ptrEnd=ptr+request->m_Payload.size();
        
    while(ptr<ptrEnd)
    {
        switch(*ptr)
        {
            case MC_RDT_CHUNK_IDS:
                ptr++;
                count=(int)mc_GetVarInt(ptr,ptrEnd-ptr,-1,&shift);
                ptr+=shift;
                if(count*size != (ptrEnd-ptr))
                {
                    goto exitlbl;
                }                
                for(int c=0;c<count;c++)
                {
                    total_size+=((mc_ChunkEntityKey*)ptr)->m_Size;
                    ptr+=size;
                }
                break;
            default:
                goto exitlbl;
        }
    }
    

    if(response->m_Payload.size() != 1+shift+total_size)
    {
        goto exitlbl;        
    }

    ptrOut=&(response->m_Payload[0]);
    if(*ptrOut != MC_RDT_CHUNKS)
    {
        goto exitlbl;                
    }
    
    ptrOut++;
    countOut=(int)mc_GetVarInt(ptrOut,1+shift+total_size,-1,&shiftOut);
    if( (countOut != count) || (shift != shiftOut) )
    {
        goto exitlbl;                        
    }
    ptrOut+=shift;
    
    ptr=ptrStart+1+shift;
    for(int c=0;c<count;c++)
    {
        sizeOut=((mc_ChunkEntityKey*)ptr)->m_Size;
        chunk=(mc_ChunkEntityKey*)ptr;
        sizeOut=chunk->m_Size;
        map <int,int>::iterator itreq = request_pairs->find(c);
        if (itreq != request_pairs->end())
        {
            collect_row=(mc_ChunkCollectorRow *)collector->m_MemPool->GetRow(itreq->second);
            uint256 hash;
            mc_gState->m_TmpBuffers->m_RpcHasher1->DoubleHash(ptrOut,sizeOut,&hash);
            if(memcmp(&hash,chunk->m_Hash,sizeof(uint256)))
            {
                goto exitlbl;                                        
            }
            chunk_err=pwalletTxsMain->m_ChunkDB->AddChunk(chunk->m_Hash,&(chunk->m_Entity),(unsigned char*)collect_row->m_TxID,collect_row->m_Vout,ptrOut,NULL,sizeOut,0,0);
            if(chunk_err)
            {
                if(chunk_err != MC_ERR_FOUND)
                {
                    goto exitlbl;                    
                }
            }
            collect_row->m_State.m_Status |= MC_CCF_DELETED;
        }        
        
        ptr+=size;
        ptrOut+=sizeOut;
    }
    
    result=true;
    
exitlbl:
                
    pRelayManager->UnLock();
                
    return result;
}

int MultichainResponseScore(mc_RelayResponse *response,mc_ChunkCollectorRow *collect_row)
{
    unsigned char *ptr;
    unsigned char *ptrEnd;
    unsigned char *ptrStart;
    int shift,count,size;
    mc_ChunkEntityKey *chunk;
    int c;
    if( (response->m_Status & MC_RST_SUCCESS) == 0 )
    {
        return MC_CCW_WORST_RESPONSE_SCORE;
    }
    
    ptrStart=&(response->m_Payload[0]);
    
    size=sizeof(mc_ChunkEntityKey);
    shift=0;
    count=0;
    
    ptr=ptrStart;
    ptrEnd=ptr+response->m_Payload.size();
    ptr++;
    count=(int)mc_GetVarInt(ptr,ptrEnd-ptr,-1,&shift);
    ptr+=shift;
    
    c=0;
    while(c<count)
    {
        chunk=(mc_ChunkEntityKey*)ptr;
        if( (memcmp(chunk->m_Hash,collect_row->m_ChunkDef.m_Hash,MC_CDB_CHUNK_HASH_SIZE) == 0) && 
            (memcmp(&(chunk->m_Entity),&(collect_row->m_ChunkDef.m_Entity),sizeof(mc_TxEntity)) == 0))
        {
            if(chunk->m_Flags & MC_CCF_ERROR_MASK)
            {
                return MC_CCW_WORST_RESPONSE_SCORE;
            }
            c=count+1;
        }
        ptr+=size;
        c++;
    }
    if(c == count)
    {
        return MC_CCW_WORST_RESPONSE_SCORE;        
    }
    return response->m_TryCount+response->m_HopCount;
}

int MultichainCollectChunks(mc_ChunkCollector* collector)
{
    uint32_t time_now;    
    vector <mc_ChunkEntityKey> vChunkDefs;
    int row,last_row,last_count;
    uint32_t total_size;
    mc_ChunkCollectorRow *collect_row;
    mc_ChunkCollectorRow *collect_subrow;
    time_now=mc_TimeNowAsUInt();
    vector<unsigned char> payload;
    unsigned char buf[16];
    int shift,count;
    unsigned char *ptrOut;
    int64_t query_id,request_id;
    map <int64_t,bool> query_to_delete;
    map <CRelayResponsePair,CRelayRequestPairs> requests_to_send;    
    map <CRelayResponsePair,CRelayRequestPairs> responses_to_process;    
    mc_RelayRequest *request;
    mc_RelayRequest *query;
    mc_RelayResponse *response;
    CRelayResponsePair response_pair;
    vector<int> vRows;
    CRelayRequestPairs request_pairs;
    int best_score,best_response,this_score,not_processed;
    
    pRelayManager->InvalidateResponsesFromDisconnected();
    
    collector->Lock();

    for(row=0;row<collector->m_MemPool->GetCount();row++)
    {
        collect_row=(mc_ChunkCollectorRow *)collector->m_MemPool->GetRow(row);
        if( (collect_row->m_State.m_Status & MC_CCF_DELETED ) == 0 )
        {
            if(collect_row->m_State.m_RequestTimeStamp <= time_now)
            {
                if(collect_row->m_State.m_Request)
                {
                    pRelayManager->DeleteRequest(collect_row->m_State.m_Request);
                    collect_row->m_State.m_Request=0;                    
                }                
            }
            request=NULL;
            if(collect_row->m_State.m_Request)
            {
                request=pRelayManager->FindRequest(collect_row->m_State.m_Request);
                if(request == NULL)
                {
                    collect_row->m_State.m_Request=0;
                    collect_row->m_State.m_RequestTimeStamp=0;
                }
            }
            if(request)
            {
                if(request->m_Responses.size())
                {
                    response_pair.request_id=collect_row->m_State.m_Request;
                    response_pair.response_id=0;
//                    printf("coll new rsp: row: %d, id: %lu, %d\n",row,collect_row->m_State.m_Request,collect_row->m_State.m_RequestPos);
                    map<CRelayResponsePair,CRelayRequestPairs>::iterator itrsp = responses_to_process.find(response_pair);
                    if (itrsp == responses_to_process.end())
                    {
                        request_pairs.m_Pairs.clear();
                        request_pairs.m_Pairs.insert(make_pair(collect_row->m_State.m_RequestPos,row));
                        responses_to_process.insert(make_pair(response_pair,request_pairs));
                    }       
                    else
                    {
                        itrsp->second.m_Pairs.insert(make_pair(collect_row->m_State.m_RequestPos,row));
                    }                    
                }            
                pRelayManager->UnLock();
            }
            else
            {
                query=NULL;
                if(collect_row->m_State.m_Query)
                {
                    query=pRelayManager->FindRequest(collect_row->m_State.m_Query);
                    if(query == NULL)
                    {
                        collect_row->m_State.m_Query=0;
                        collect_row->m_State.m_QueryNextAttempt=time_now+MultichainNextChunkQueryAttempt(collect_row->m_State.m_QueryAttempts);                                                
                        collect_row->m_State.m_Status |= MC_CCF_UPDATED;
                    }
                }
                if(query)
                {
                    best_response=-1;
                    best_score=MC_CCW_WORST_RESPONSE_SCORE;
                    for(int i=0;i<(int)query->m_Responses.size();i++)
                    {
                        this_score=MultichainResponseScore(&(query->m_Responses[i]),collect_row);
                        if(this_score < best_score)
                        {
                            best_score=this_score;
                            best_response=i;
                        }
                    }
//                    printf("coll new req: row: %d, id:  %lu, rsps: %d, score (%d,%d)\n",row,collect_row->m_State.m_Query,(int)query->m_Responses.size(),best_score,best_response);
                    if(best_response >= 0)
                    {
                        response_pair.request_id=collect_row->m_State.m_Query;
                        response_pair.response_id=best_response;                        
                        map<CRelayResponsePair,CRelayRequestPairs>::iterator itrsp = requests_to_send.find(response_pair);
                        if (itrsp == requests_to_send.end())
                        {
                            request_pairs.m_Pairs.clear();
                            request_pairs.m_Pairs.insert(make_pair(row,0));
                            requests_to_send.insert(make_pair(response_pair,request_pairs));
                        }       
                        else
                        {
                            itrsp->second.m_Pairs.insert(make_pair(row,0));
                        }                    
                    }
                }
                pRelayManager->UnLock();
            }
        }        
    }

    BOOST_FOREACH(PAIRTYPE(const CRelayResponsePair, CRelayRequestPairs)& item, responses_to_process)    
    {
        MultichainProcessChunkResponse(&(item.first),&(item.second.m_Pairs),collector);
    }
    
    BOOST_FOREACH(PAIRTYPE(const CRelayResponsePair, CRelayRequestPairs)& item, requests_to_send)    
    {
        payload.clear();
        shift=mc_PutVarInt(buf,16,requests_to_send.size());
        payload.resize(1+shift+sizeof(mc_ChunkEntityKey)*requests_to_send.size());
        ptrOut=&(payload[0]);
        *ptrOut=MC_RDT_CHUNK_IDS;
        ptrOut++;
        memcpy(ptrOut,buf,shift);
        ptrOut+=shift;
        count=0;
        BOOST_FOREACH(PAIRTYPE(const int, int)& chunk_row, item.second.m_Pairs)    
        {                
            collect_subrow=(mc_ChunkCollectorRow *)collector->m_MemPool->GetRow(chunk_row.first);
            collect_subrow->m_State.m_RequestPos=count;
            memcpy(ptrOut,&(collect_subrow->m_ChunkDef),sizeof(mc_ChunkEntityKey));
            ptrOut+=sizeof(mc_ChunkEntityKey);
            count++;
        }
        
        request=pRelayManager->FindRequest(item.first.request_id);
        if(request == NULL)
        {
            return false;
        }

        response=&(request->m_Responses[item.first.response_id]);
        request_id=pRelayManager->SendNextRequest(response,MC_RMT_CHUNK_REQUEST,0,payload);
        if(fDebug)LogPrint("chunks","New chunk request %ld, response: %ld, chunks: %d\n",request_id,response->m_Nonce,item.second.m_Pairs.size());
        BOOST_FOREACH(PAIRTYPE(const int, int)& chunk_row, item.second.m_Pairs)    
        {                
            collect_subrow=(mc_ChunkCollectorRow *)collector->m_MemPool->GetRow(chunk_row.first);
            collect_subrow->m_State.m_Request=request_id;
            collect_row->m_State.m_RequestTimeStamp=time_now+MC_CCW_TIMEOUT_REQUEST;
        }
    }

    row=0;
    last_row=0;
    last_count=0;
    total_size=0;
    while(row<=collector->m_MemPool->GetCount())
    {
        collect_row=NULL;
        if(row<collector->m_MemPool->GetCount())
        {
            collect_row=(mc_ChunkCollectorRow *)collector->m_MemPool->GetRow(row);
        }
        
        if( (collect_row == NULL)|| (last_count >= MC_CCW_MAX_CHUNKS_PER_QUERY) || (total_size+collect_row->m_ChunkDef.m_Size > MAX_SIZE-48) )
        {
            if(last_count)
            {
                payload.clear();
                shift=mc_PutVarInt(buf,16,last_count);
                payload.resize(1+shift+sizeof(mc_ChunkEntityKey)*last_count);
                ptrOut=&(payload[0]);
                
                *ptrOut=MC_RDT_CHUNK_IDS;
                ptrOut++;
                memcpy(ptrOut,buf,shift);
                ptrOut+=shift;
                for(int r=last_row;r<row;r++)
                {
                    collect_subrow=(mc_ChunkCollectorRow *)collector->m_MemPool->GetRow(r);
                    if(collect_subrow->m_State.m_Status & MC_CCF_SELECTED)
                    {
                        memcpy(ptrOut,&(collect_subrow->m_ChunkDef),sizeof(mc_ChunkEntityKey));
                        ptrOut+=sizeof(mc_ChunkEntityKey);
                    }
                }
                query_id=pRelayManager->SendRequest(NULL,MC_RMT_CHUNK_QUERY,0,payload);
                if(fDebug)LogPrint("chunks","New chunk query: %ld, chunks: %d\n",query_id,last_count);
                for(int r=last_row;r<row;r++)
                {
                    collect_subrow=(mc_ChunkCollectorRow *)collector->m_MemPool->GetRow(r);
                    if(collect_subrow->m_State.m_Status & MC_CCF_SELECTED)
                    {
//                        printf("coll new qry: row: %d, id: %lu: att: %d\n",r,query_id,collect_subrow->m_State.m_QueryAttempts);
                        collect_subrow->m_State.m_Status -= MC_CCF_SELECTED;
                        collect_subrow->m_State.m_Query=query_id;
                        collect_subrow->m_State.m_QueryAttempts+=1;
                        collect_subrow->m_State.m_QueryTimeStamp=time_now+MC_CCW_TIMEOUT_QUERY;
                        collect_subrow->m_State.m_Status |= MC_CCF_UPDATED;
                    }
                }
                last_row=row;
                last_count=0;     
                total_size=0;
            }
        }
        
        if(collect_row)
        {
            if( (collect_row->m_State.m_Status & MC_CCF_DELETED ) == 0 )
            {
                if(collect_row->m_State.m_QueryTimeStamp <= time_now)
                {
                    if(collect_row->m_State.m_Request)
                    {
                        pRelayManager->DeleteRequest(collect_row->m_State.m_Request);
                        collect_row->m_State.m_Request=0;
                    }
                    if(collect_row->m_State.m_Query)
                    {
                        map<int64_t, bool>::iterator itqry = query_to_delete.find(collect_row->m_State.m_Query);
                        if (itqry == query_to_delete.end())
                        {
                            query_to_delete.insert(make_pair(collect_row->m_State.m_Query,true));
                        }       
                        collect_row->m_State.m_Query=0;
                        collect_row->m_State.m_QueryNextAttempt=time_now+MultichainNextChunkQueryAttempt(collect_row->m_State.m_QueryAttempts);      
                        collect_row->m_State.m_Status |= MC_CCF_UPDATED;
                    }
                    if(collect_row->m_State.m_QueryNextAttempt <= time_now)
                    {
                        if( (collect_row->m_State.m_Status & MC_CCF_ERROR_MASK) == 0)
                        {
                            collect_row->m_State.m_Status |= MC_CCF_SELECTED;
                            last_count++;
                        }
                    }
                }
            }
        }
        row++;
    }
        
    not_processed=0;
    
    for(row=0;row<collector->m_MemPool->GetCount();row++)
    {
        collect_row=(mc_ChunkCollectorRow *)collector->m_MemPool->GetRow(row);
        if( (collect_row->m_State.m_Status & MC_CCF_DELETED ) == 0 )
        {
            if(collect_row->m_State.m_Query)
            {
                map<int64_t, bool>::iterator itqry = query_to_delete.find(collect_row->m_State.m_Query);
                if (itqry != query_to_delete.end())
                {
                    itqry->second=false;
                }            
            }
            not_processed++;
        }
    }

    BOOST_FOREACH(PAIRTYPE(const int64_t, bool)& item, query_to_delete)    
    {
        if(item.second)
        {
            pRelayManager->DeleteRequest(item.first);
        }
    }    
    
    collector->UnLock();
    
    return not_processed;
}


void mc_RelayPayload_ChunkIDs(vector<unsigned char>* payload,vector <mc_ChunkEntityKey>& vChunkDefs,int size)
{
    unsigned char buf[16];
    int shift;
    unsigned char *ptrOut;
    
    if(payload)
    {
        if(vChunkDefs.size())
        {
            shift=mc_PutVarInt(buf,16,vChunkDefs.size());
            payload->resize(1+shift+size*vChunkDefs.size());
            ptrOut=&(*payload)[0];

            *ptrOut=MC_RDT_CHUNK_IDS;
            ptrOut++;
            memcpy(ptrOut,buf,shift);
            ptrOut+=shift;

            for(int i=0;i<(int)vChunkDefs.size();i++)
            {
                memcpy(ptrOut,&vChunkDefs[i],size);
                ptrOut+=size;
            }                        
        }
    }
}

bool mc_RelayProcess_Chunk_Query(unsigned char *ptrStart,unsigned char *ptrEnd,vector<unsigned char>* payload_response,vector<unsigned char>* payload_relay,string& strError)
{
    unsigned char *ptr;
    int shift,count,size;
    vector <mc_ChunkEntityKey> vToRelay;
    vector <mc_ChunkEntityKey> vToRespond;
    mc_ChunkEntityKey chunk;
    mc_ChunkDBRow chunk_def;
    
    size=sizeof(mc_ChunkEntityKey);
    ptr=ptrStart;
    while(ptr<ptrEnd)
    {
        switch(*ptr)
        {
            case MC_RDT_CHUNK_IDS:
                ptr++;
                count=(int)mc_GetVarInt(ptr,ptrEnd-ptr,-1,&shift);
                ptr+=shift;
                if(count*size != (ptrEnd-ptr))
                {
                    strError="Bad chunk ids request";
                    return false;                    
                }                
                for(int c=0;c<count;c++)
                {
                    chunk=*(mc_ChunkEntityKey*)ptr;
                    if(pwalletTxsMain->m_ChunkDB->GetChunkDef(&chunk_def,chunk.m_Hash,NULL,NULL,-1) == MC_ERR_NOERROR)
                    {
                        if(chunk_def.m_Size != chunk.m_Size)
                        {
                            chunk.m_Flags |= MC_CCF_WRONG_SIZE;
                        }
                        vToRespond.push_back(chunk);
                    }                    
                    else
                    {
                        vToRelay.push_back(chunk);                                                
                    }
                    ptr+=size;
                }

                mc_RelayPayload_ChunkIDs(payload_response,vToRespond,size);
                mc_RelayPayload_ChunkIDs(payload_relay,vToRelay,size);
                break;
            default:
                strError=strprintf("Unsupported request format (%d, %d)",MC_RMT_CHUNK_QUERY,*ptr);
                return false;
        }
    }
    
    return true;
}

bool mc_RelayProcess_Chunk_Request(unsigned char *ptrStart,unsigned char *ptrEnd,vector<unsigned char>* payload_response,vector<unsigned char>* payload_relay,string& strError)
{
    unsigned char *ptr;
    int shift,count,size;
    mc_ChunkEntityKey chunk;
    mc_ChunkDBRow chunk_def;
    const unsigned char *chunk_found;
    unsigned char buf[16];
    size_t chunk_bytes;
    unsigned char *ptrOut;
    
    uint32_t total_size=0;
    
    mc_gState->m_TmpBuffers->m_RelayTmpBuffer->Clear();
    mc_gState->m_TmpBuffers->m_RelayTmpBuffer->AddElement();
            
    size=sizeof(mc_ChunkEntityKey);
    ptr=ptrStart;
    while(ptr<ptrEnd)
    {
        switch(*ptr)
        {
            case MC_RDT_CHUNK_IDS:
                ptr++;
                count=(int)mc_GetVarInt(ptr,ptrEnd-ptr,-1,&shift);
                ptr+=shift;
                if(count*size != (ptrEnd-ptr))
                {
                    strError="Bad chunk ids request";
                    return false;                    
                }                
                for(int c=0;c<count;c++)
                {
                    chunk=*(mc_ChunkEntityKey*)ptr;
                    if(pwalletTxsMain->m_ChunkDB->GetChunkDef(&chunk_def,chunk.m_Hash,NULL,NULL,-1) == MC_ERR_NOERROR)
                    {
                        if(chunk_def.m_Size != chunk.m_Size)
                        {
                            strError="Bad chunk size";
                            return false;                    
                        }
                        if(total_size + chunk_def.m_Size > MAX_SIZE-48)
                        {
                            strError="Total size of requested chunks is too big";
                            return false;                                                
                        }
                        chunk_found=pwalletTxsMain->m_ChunkDB->GetChunk(&chunk_def,0,-1,&chunk_bytes);
                        mc_gState->m_TmpBuffers->m_RelayTmpBuffer->SetData(chunk_found,chunk_bytes);
                    }                    
                    else
                    {
                        strError="Chunk not found";
                        return false;                    
                    }
                    ptr+=size;
                }
                
                chunk_found=mc_gState->m_TmpBuffers->m_RelayTmpBuffer->GetData(0,&chunk_bytes);
                shift=mc_PutVarInt(buf,16,count);
                payload_response->resize(1+shift+chunk_bytes);
                ptrOut=&(*payload_response)[0];
                
                *ptrOut=MC_RDT_CHUNKS;
                ptrOut++;
                memcpy(ptrOut,buf,shift);
                ptrOut+=shift;
                memcpy(ptrOut,chunk_found,chunk_bytes);
                ptrOut+=chunk_bytes;
                
                break;
            default:
                strError=strprintf("Unsupported request format (%d, %d)",MC_RMT_CHUNK_QUERY,*ptr);
                return false;
        }
    }
    
    return true;
}

bool mc_RelayProcess_Address_Query(unsigned char *ptrStart,unsigned char *ptrEnd,vector<unsigned char>* payload_response,vector<unsigned char>* payload_relay,string& strError)
{
    unsigned char *ptr;
    unsigned char *ptrOut;
    unsigned char buf[16];
    int shift;
    CKey key;
    
    ptr=ptrStart;
    while(ptr<ptrEnd)
    {
        switch(*ptr)
        {
            case MC_RDT_MC_ADDRESS:
                ptr++;
                if(sizeof(CKeyID) != (ptrEnd-ptr))
                {
                    strError="Bad address query request";
                    return false;
                }

                if(pwalletMain->GetKey(*(CKeyID*)ptr, key))
                {
                    if(payload_response)
                    {
                        shift=mc_PutVarInt(buf,16,pRelayManager->m_MyAddress.m_NetAddresses.size());
                        payload_response->resize(1+sizeof(CKeyID)+1+shift+sizeof(CAddress)*pRelayManager->m_MyAddress.m_NetAddresses.size());
                        ptrOut=&(*payload_response)[0];
                        
                        *ptrOut=MC_RDT_MC_ADDRESS;
                        ptrOut++;
                        *(CKeyID*)ptrOut=pRelayManager->m_MyAddress.m_Address;
                        ptrOut+=sizeof(CKeyID);
                        
                        *ptrOut=MC_RDT_NET_ADDRESS;
                        ptrOut++;
                        memcpy(ptrOut,buf,shift);
                        ptrOut+=shift;
                        for(int i=0;i<(int)pRelayManager->m_MyAddress.m_NetAddresses.size();i++)
                        {
                            memcpy(ptrOut,&(pRelayManager->m_MyAddress.m_NetAddresses[i]),sizeof(CAddress));
                            ptrOut+=sizeof(CAddress);
                        }                        
                    }
                }
                else
                {
                    if(payload_relay)
                    {
                        payload_relay->resize(ptrEnd-ptrStart);
                        memcpy(&(*payload_relay)[0],ptrStart,ptrEnd-ptrStart);                    
                    }
                }
                
                ptr+=sizeof(CKeyID);
                break;
            default:
                strError=strprintf("Unsupported request format (%d, %d)",MC_RMT_MC_ADDRESS_QUERY,*ptr);
                return false;
        }
    }
    
    return true;
}

bool MultichainRelayResponse(uint32_t msg_type_stored, CNode *pto_stored,
                             uint32_t msg_type_in, uint32_t  flags, vector<unsigned char>& vPayloadIn,vector<CKeyID>&  vAddrIn,
                             uint32_t* msg_type_response,uint32_t  *flags_response,vector<unsigned char>& vPayloadResponse,vector<CKeyID>&  vAddrResponse,
                             uint32_t* msg_type_relay,uint32_t  *flags_relay,vector<unsigned char>& vPayloadRelay,vector<CKeyID>&  vAddrRelay,string& strError)
{
    unsigned char *ptr;
    unsigned char *ptrEnd;
    vector<unsigned char> *payload_relay_ptr=NULL;
    vector<unsigned char> *payload_response_ptr=NULL;
    
    if(msg_type_response)
    {
        payload_response_ptr=&vPayloadResponse;
    }
    
    if(msg_type_relay)
    {
        payload_relay_ptr=&vPayloadRelay;
    }
    
    ptr=&vPayloadIn[0];
    ptrEnd=ptr+vPayloadIn.size();
            
//    mc_DumpSize("H",ptr,ptrEnd-ptr,32);
    strError="";
    switch(msg_type_in)
    {
        case MC_RMT_MC_ADDRESS_QUERY:
            if(msg_type_stored)
            {
                strError=strprintf("Unexpected response message type (%d,%d)",msg_type_stored,msg_type_in);;
                goto exitlbl;
            } 
            if(payload_response_ptr == NULL)
            {
                if(payload_relay_ptr)
                {
                    vPayloadRelay=vPayloadIn;
                    *msg_type_relay=msg_type_in;                    
                }
            }
            else
            {
                if(mc_RelayProcess_Address_Query(ptr,ptrEnd,payload_response_ptr,payload_relay_ptr,strError))
                {
                    if(payload_response_ptr && (payload_response_ptr->size() != 0))
                    {
                        *msg_type_response=MC_RMT_NODE_DETAILS;
                    }
                    if(payload_relay_ptr && (payload_relay_ptr->size() != 0))
                    {
                        *msg_type_relay=MC_RMT_MC_ADDRESS_QUERY;
                    }
                }
            }
            break;
        case MC_RMT_NODE_DETAILS:
            if(msg_type_stored != MC_RMT_MC_ADDRESS_QUERY)
            {
                strError=strprintf("Unexpected response message type (%d,%d)",msg_type_stored,msg_type_in);;
                goto exitlbl;
            } 
            if(payload_response_ptr)
            {
                *msg_type_response=MC_RMT_ADD_RESPONSE;
            }

            break;
        case MC_RMT_CHUNK_QUERY:
            if(msg_type_stored)
            {
                strError=strprintf("Unexpected response message type (%d,%d)",msg_type_stored,msg_type_in);;
                goto exitlbl;
            } 
            if(payload_response_ptr == NULL)
            {
                if(payload_relay_ptr)
                {
                    vPayloadRelay=vPayloadIn;
                    *msg_type_relay=msg_type_in;                    
                }
            }
            else
            {
                if(mc_RelayProcess_Chunk_Query(ptr,ptrEnd,payload_response_ptr,payload_relay_ptr,strError))
                {
                    if(payload_response_ptr && (payload_response_ptr->size() != 0))
                    {
                        *msg_type_response=MC_RMT_CHUNK_QUERY_HIT;
                    }
                    if(payload_relay_ptr && (payload_relay_ptr->size() != 0))
                    {
                        *msg_type_relay=MC_RMT_CHUNK_QUERY;
                    }
                }
            }            
            break;
        case MC_RMT_CHUNK_QUERY_HIT:
            if(msg_type_stored != MC_RMT_CHUNK_QUERY)
            {
                strError=strprintf("Unexpected response message type (%d,%d)",msg_type_stored,msg_type_in);;
                goto exitlbl;
            } 
            if(payload_response_ptr)
            {
                *msg_type_response=MC_RMT_ADD_RESPONSE;
            }
            break;
        case MC_RMT_CHUNK_REQUEST:
            if(msg_type_stored != MC_RMT_CHUNK_QUERY_HIT)
            {
                strError=strprintf("Unexpected response message type (%d,%d)",msg_type_stored,msg_type_in);;
                goto exitlbl;
            } 
            if(payload_response_ptr == NULL)
            {
                if(payload_relay_ptr)
                {
                    vPayloadRelay=vPayloadIn;
                    *msg_type_relay=msg_type_in;                    
                }
            }
            else
            {
                if(mc_RelayProcess_Chunk_Request(ptr,ptrEnd,payload_response_ptr,payload_relay_ptr,strError))
                {
                    if(payload_response_ptr && (payload_response_ptr->size() != 0))
                    {
                        *msg_type_response=MC_RMT_CHUNK_RESPONSE;
                    }
                    if(payload_relay_ptr && (payload_relay_ptr->size() != 0))
                    {
                        *msg_type_relay=MC_RMT_CHUNK_REQUEST;
                    }
                }
            }            
            break;
        case MC_RMT_CHUNK_RESPONSE:
            if(msg_type_stored != MC_RMT_CHUNK_REQUEST)
            {
                strError=strprintf("Unexpected response message type (%d,%d)",msg_type_stored,msg_type_in);;
                goto exitlbl;
            } 
            if(payload_response_ptr)
            {
                *msg_type_response=MC_RMT_ADD_RESPONSE;
            }

            break;
    }
    
exitlbl:
            
    if(strError.size())
    {
        return false;
    }

    return true;
}

void mc_Limiter::Zero()
{
    memset(this,0,sizeof(mc_Limiter));    
}

int mc_Limiter::Initialize(int seconds,int measures)
{
    Zero();
    
    if(seconds > MC_LIM_MAX_SECONDS)
    {
        return MC_ERR_INVALID_PARAMETER_VALUE;
    }
    if(measures > MC_LIM_MAX_MEASURES)
    {
        return MC_ERR_INVALID_PARAMETER_VALUE;
    }
    m_SecondCount=seconds;
    m_MeasureCount=measures;
    m_Time=mc_TimeNowAsUInt();
    
    return MC_ERR_NOERROR;
}

int mc_Limiter::SetLimit(int meausure, int64_t limit)
{
    if( (meausure > MC_LIM_MAX_MEASURES) || (meausure <0) )
    {
        return MC_ERR_INVALID_PARAMETER_VALUE;        
    }
    m_Limits[meausure]=limit*m_SecondCount;
    return MC_ERR_NOERROR;    
}

void mc_Limiter::CheckTime()
{
    CheckTime(mc_TimeNowAsUInt());
}

void mc_Limiter::CheckTime(uint32_t time_now)
{
    if(m_SecondCount == 0)
    {
        return;        
    }
    if(time_now == m_Time)
    {
        return;
    }
    if(time_now >= m_Time + m_SecondCount)
    {
        memset(m_Totals,0,MC_LIM_MAX_MEASURES);
        memset(m_Measures,0,MC_LIM_MAX_MEASURES*MC_LIM_MAX_SECONDS);
    }
    
    for(uint32_t t=m_Time;t<time_now;t++)
    {
        int p=(t+1)%m_SecondCount;
        for(int m=0;m<m_MeasureCount;m++)
        {
            m_Totals[m]-=m_Measures[m*MC_LIM_MAX_SECONDS+p];
            m_Measures[m*MC_LIM_MAX_SECONDS+p]=0;
        }
    }
    
    m_Time=time_now;
}

void mc_Limiter::SetEvent(int64_t m1)
{
    SetEvent(m1,0,0,0);
}

void mc_Limiter::SetEvent(int64_t m1,int64_t m2)
{
    SetEvent(m1,m2,0,0);    
}

void mc_Limiter::SetEvent(int64_t m1,int64_t m2,int64_t m3)
{
    SetEvent(m1,m2,m3,0);        
}

void mc_Limiter::SetEvent(int64_t m1,int64_t m2,int64_t m3,int64_t m4)
{
    m_Event[0]=m1;
    m_Event[1]=m2;
    m_Event[2]=m3;
    m_Event[3]=m4;
}

int mc_Limiter::Disallowed()
{
    return Disallowed(mc_TimeNowAsUInt());
}

int mc_Limiter::Disallowed(uint32_t t)
{
    CheckTime(t);
    
    for(int m=0;m<m_MeasureCount;m++)
    {
        if(m_Totals[m]+m_Event[m] > m_Limits[m])
        {
            return 1;
        }
    }
    
    return 0;
}

void mc_Limiter::Increment()
{
    if(m_SecondCount == 0)
    {
        return;        
    }
    
    int p=(m_Time+1)%m_SecondCount;
    for(int m=0;m<m_MeasureCount;m++)
    {
        m_Totals[m]+=m_Event[m];
        m_Measures[m*MC_LIM_MAX_SECONDS+p]+=m_Event[m];
    }    
}

void mc_NodeFullAddress::Zero()
{
    m_Address=CKeyID(0);
    m_NetAddresses.clear();
}

void mc_RelayManager::SetMyIPs(uint32_t *ips,int ip_count)
{
    m_MyIPCount=ip_count;
    for(int i=0;i<ip_count;i++)
    {
        m_MyIPs[i]=ips[i];
    }
}

void mc_RelayManager::InitNodeAddress(mc_NodeFullAddress *node_address,CNode* pto,uint32_t action)
{
    uint32_t pto_address_local;
    in_addr addr;
    CKey key;
    CPubKey pkey;            
    bool key_found=false;
    
    if(action & MC_PRA_MY_ORIGIN_MC_ADDRESS)
    {
        if(pto)
        {
            node_address->m_Address=pto->kAddrLocal;        
        }
    }
    else
    {
        if(pto == NULL)
        {
            if(mapArgs.count("-handshakelocal"))
            {
                CBitcoinAddress address(mapArgs["-handshakelocal"]);
                if (address.IsValid())    
                {
                    CTxDestination dst=address.Get();
                    CKeyID *lpKeyID=boost::get<CKeyID> (&dst);
                    if(lpKeyID)
                    {
                        if(pwalletMain->GetKey(*lpKeyID, key))
                        {
                            node_address->m_Address=*lpKeyID;
                            key_found=true;
                        }
                    }
                }        
            }

            if(!key_found)
            {
                if(!pwalletMain->GetKeyFromAddressBook(pkey,MC_PTP_CONNECT))
                {
                    pkey=pwalletMain->vchDefaultKey;
                }
                node_address->m_Address=pkey.GetID();
            }  
        }
    }
    
    node_address->m_NetAddresses.clear();
    
    pto_address_local=0;
    if(action & MC_PRA_MY_ORIGIN_NT_ADDRESS)
    {
        node_address->m_NetAddresses.push_back(CAddress(pto->addrLocal));

        if(pto->addrLocal.IsIPv4())
        {
            pto_address_local=(pto->addrLocal.GetByte(3)<<24)+(pto->addrLocal.GetByte(2)<<16)+(pto->addrLocal.GetByte(1)<<8)+pto->addrLocal.GetByte(0);            
            addr.s_addr=pto_address_local;
            node_address->m_NetAddresses.push_back(CAddress(CService(addr,GetListenPort())));
        }
    }
    
    
    for(int i=0;i<m_MyIPCount;i++)
    {
        if(m_MyIPs[i] != pto_address_local)
        {
            addr.s_addr=htonl(m_MyIPs[i]);
            node_address->m_NetAddresses.push_back(CAddress(CService(addr,GetListenPort())));
        }
    }        
}

void mc_RelayManager::InitNodeAddress(mc_NodeFullAddress *node_address,CKeyID& mc_address, vector<CAddress>& net_addresses)
{
    node_address->m_Address=mc_address;
    node_address->m_NetAddresses=net_addresses;
}

void mc_RelayManager::MsgTypeSettings(uint32_t msg_type,int latency,int seconds,int64_t serves_per_second,int64_t bytes_per_second)
{
    mc_Limiter limiter;
    
    map<uint32_t, int>::iterator itlat = m_Latency.find(msg_type);
    if (itlat == m_Latency.end())
    {
        m_Latency.insert(make_pair(msg_type,latency));
    }                    
    else
    {
        itlat->second=latency;
    }
    
    limiter.Initialize(seconds,2);
    limiter.SetLimit(0,serves_per_second);
    limiter.SetLimit(1,bytes_per_second);
    
    map<uint32_t, mc_Limiter>::iterator itlim = m_Limiters.find(msg_type);
    if (itlim == m_Limiters.end())
    {
        m_Limiters.insert(make_pair(msg_type,limiter));
    }                    
    else
    {
        itlim->second=limiter;
    }
}

int64_t mc_RelayManager::AggregateNonce(uint32_t timestamp,uint32_t nonce)
{
    return ((int64_t)nonce<<32)+(int64_t)timestamp;
}

uint32_t mc_RelayManager::Timestamp(int64_t aggr_nonce)
{
    return aggr_nonce & 0xFFFFFFFF;
}

uint32_t mc_RelayManager::Nonce(int64_t aggr_nonce)
{
    return (aggr_nonce >> 32) & 0xFFFFFFFF;    
}


void mc_RelayManager::Zero()
{
    m_Semaphore=NULL;
    m_LockedBy=0;         
}

void mc_RelayManager::Destroy()
{
    if(m_Semaphore)
    {
        __US_SemDestroy(m_Semaphore);
    }
    
    Zero();    
}

int mc_RelayManager::Initialize()
{
    m_Semaphore=__US_SemCreate();
    InitNodeAddress(&m_MyAddress,NULL,MC_PRA_NONE);
    SetDefaults();
    return MC_ERR_NOERROR;    
}

int mc_RelayManager::Lock(int write_mode,int allow_secondary)
{        
    uint64_t this_thread;
    this_thread=__US_ThreadID();
    
    if(this_thread == m_LockedBy)
    {
        return allow_secondary;
    }
    
    __US_SemWait(m_Semaphore); 
    m_LockedBy=this_thread;
    
    return 0;
}

void mc_RelayManager::UnLock()
{    
    m_LockedBy=0;
    __US_SemPost(m_Semaphore);
}

int mc_RelayManager::Lock()
{        
    return Lock(1,0);
}

void mc_RelayManager::SetDefaults()
{
    MsgTypeSettings(MC_RMT_NONE            , 0,10,1000,100*1024*1024);
    MsgTypeSettings(MC_RMT_MC_ADDRESS_QUERY,10,10, 100,  1*1024*1024);
    MsgTypeSettings(MC_RMT_NODE_DETAILS    , 0,10, 100,  1*1024*1024);
    MsgTypeSettings(MC_RMT_REJECT          , 0,10,1000,  1*1024*1024);
    MsgTypeSettings(MC_RMT_CHUNK_QUERY     ,10,10, 100,  1*1024*1024);
    MsgTypeSettings(MC_RMT_CHUNK_QUERY_HIT ,30,10, 100,  1*1024*1024);
    MsgTypeSettings(MC_RMT_CHUNK_REQUEST   ,30,10, 100,  1*1024*1024);
    MsgTypeSettings(MC_RMT_CHUNK_RESPONSE  , 0,10, 100,100*1024*1024);
    MsgTypeSettings(MC_RMT_ERROR_IN_MESSAGE,30,10,1000,  1*1024*1024);
    MsgTypeSettings(MC_RMT_NEW_REQUEST     ,30,10,1000,  1*1024*1024);
    
    
    m_MinTimeShift=180;
    m_MaxTimeShift=180;
    m_MaxResponses=16;
}

void mc_RelayManager::CheckTime()
{
    uint32_t time_now=mc_TimeNowAsUInt();
    if(time_now == m_LastTime)
    {
        return;
    }
    
    for(map<mc_RelayRecordKey,mc_RelayRecordValue>::iterator it = m_RelayRecords.begin(); it != m_RelayRecords.end();)
    {
        if(it->second.m_Timestamp < m_LastTime)
        {
            m_RelayRecords.erase(it++);
        }
        else
        {
            it++;
        }
    }        
    m_LastTime=time_now;
}

void mc_RelayManager::SetRelayRecord(CNode *pto,CNode *pfrom,uint32_t msg_type,uint32_t timestamp,uint32_t nonce)
{
    map<uint32_t, int>::iterator itlat = m_Latency.find(msg_type);
    if (itlat == m_Latency.end())
    {
        return;
    }                    
    if(itlat->second <= 0 )
    {
        return;        
    }    
    
    NodeId pto_id=0;
    if(pto)
    {
        pto_id=pto->GetId();
    }
    const mc_RelayRecordKey key=mc_RelayRecordKey(timestamp,nonce,pto_id);
    mc_RelayRecordValue value;
    value.m_NodeFrom=0;
    if(pfrom)
    {
        value.m_NodeFrom=pfrom->GetId();
    }
    value.m_MsgType=msg_type;
    value.m_Timestamp=m_LastTime+itlat->second;
    value.m_Count=1;
    
    map<const mc_RelayRecordKey, mc_RelayRecordValue>::iterator it = m_RelayRecords.find(key);
    if (it == m_RelayRecords.end())
    {
        m_RelayRecords.insert(make_pair(key,value));
    }                    
    else
    {
        value.m_Timestamp=(it->second).m_Timestamp;
        value.m_Count=(it->second).m_Count;
        it->second=value;
    }    
//    printf("setrr: %d, ts: %u, nc: %u, mt: %d\n",pto_id,value.m_Timestamp,nonce,msg_type);
}

int mc_RelayManager::GetRelayRecord(CNode *pfrom,uint32_t timestamp,uint32_t nonce,uint32_t* msg_type,CNode **pto)
{
    NodeId pfrom_id,pto_id;
    
    pfrom_id=0;
    if(pfrom)
    {
        pfrom_id=pfrom->GetId();
    }
//    printf("getrr: %d, ts: %u, nc: %u\n",pfrom_id,timestamp,nonce);
    const mc_RelayRecordKey key=mc_RelayRecordKey(timestamp,nonce,pfrom_id);
    map<mc_RelayRecordKey, mc_RelayRecordValue>::iterator it = m_RelayRecords.find(key);
    if (it == m_RelayRecords.end())
    {
        return MC_ERR_NOT_FOUND;
    }
    
    if(it->second.m_MsgType == MC_RMT_ERROR_IN_MESSAGE)
    {
        return MC_ERR_ERROR_IN_SCRIPT;
    }
    
    if(msg_type)
    {
        *msg_type=it->second.m_MsgType;        
    }
    
    
    if(pto)
    {
        pto_id=it->second.m_NodeFrom;
    
        if(pto_id)
        {
            LOCK(cs_vNodes);
            BOOST_FOREACH(CNode* pnode, vNodes)
            {
                if(pnode->GetId() == pto_id)
                {
                    *pto=pnode;
                    return MC_ERR_NOERROR;
                }
            }
        }
        else
        {
            return MC_ERR_NOERROR;            
        }
    }
    else
    {
        it->second.m_Count+=1;
        if(it->second.m_Count > m_MaxResponses)
        {
            return MC_ERR_NOT_ALLOWED;            
        }
        return MC_ERR_NOERROR;                    
    }
    
    return MC_ERR_NOT_ALLOWED;
}

uint32_t mc_RelayManager::GenerateNonce()
{
    uint32_t nonce;     
    
    GetRandBytes((unsigned char*)&nonce, sizeof(nonce));
    
    nonce &= 0x7FFFFFFF;
    
    return nonce;
}

int64_t mc_RelayManager::PushRelay(CNode*    pto, 
                                uint32_t  msg_format,        
                                vector <int32_t> &vHops,
                                uint32_t  msg_type,
                                uint32_t  timestamp_to_send,
                                uint32_t  nonce_to_send,
                                uint32_t  timestamp_to_respond,
                                uint32_t  nonce_to_respond,
                                uint32_t  flags,
                                vector<unsigned char>& payload,
                                vector<CScript>&  sigScripts_to_relay,
                                CNode*    pfrom, 
                                uint32_t  action)
{
    vector <unsigned char> vOriginAddress;
    vector <unsigned char> vDestinationAddress;    
    vector<CScript>  sigScripts;
    CScript sigScript;
    uint256 message_hash;
    uint32_t timestamp;     
    uint32_t nonce;     
    
    nonce=nonce_to_send;
    timestamp=timestamp_to_send;
    
    if(action & MC_PRA_GENERATE_TIMESTAMP)
    {
        timestamp=mc_TimeNowAsUInt();
    }
    
    if(action & MC_PRA_GENERATE_NONCE)
    {
        nonce=GenerateNonce();
    }
    
    int64_t aggr_nonce=AggregateNonce(timestamp,nonce);
    
    if( (action & MC_PRA_SIGN_WITH_HANDSHAKE_ADDRESS) && (MCP_ANYONE_CAN_CONNECT == 0) )
    {
        CHashWriter ssHash(SER_GETHASH, 0);
        ssHash << msg_type;        
        ssHash << timestamp;
        ssHash << nonce;
        ssHash << timestamp_to_respond;
        ssHash << nonce_to_respond;
        ssHash << flags;
        ssHash << payload;
        
        message_hash=ssHash.GetHash();    
        
        CHashWriter ssSig(SER_GETHASH, 0);
        
        ssSig << message_hash;
        ssSig << vector<unsigned char>((unsigned char*)&aggr_nonce, (unsigned char*)&aggr_nonce+sizeof(aggr_nonce));
        uint256 signed_hash=ssSig.GetHash();
        CKey key;
        CKeyID keyID;
        CPubKey pkey;            

        keyID=pto->kAddrLocal;
        
        if(pwalletMain->GetKey(keyID, key))
        {
            pkey=key.GetPubKey();
            vector<unsigned char> vchSig;
            sigScript.clear();
            if (key.Sign(signed_hash, vchSig))
            {
                vchSig.push_back(0x00);
                sigScript << vchSig;
                sigScript << ToByteVector(pkey);
            }
            else
            {
                LogPrintf("PushRelay(): Internal error: Cannot sign\n");                
            }
        }
        else
        {
            LogPrintf("PushRelay(): Internal error: Cannot find key for signature\n");
        }
        sigScripts.push_back(sigScript);
    }    
    
    for(unsigned int i=0;i<sigScripts_to_relay.size();i++)
    {
        sigScripts.push_back(sigScripts_to_relay[i]);
    }
    
    if(pfrom)
    {
        vHops.push_back((int32_t)pfrom->GetId());
    }
    
//    printf("send: %d, to: %d, from: %d, hc: %d, size: %d, ts: %u, nc: %u\n",msg_type,pto->GetId(),pfrom ? pfrom->GetId() : 0,(int)vHops.size(),(int)payload.size(),timestamp,nonce);
    if(fDebug)LogPrint("offchain","Offchain send: %ld, request: %ld, to: %d, from: %d, msg: %d, hops: %d\n",
            AggregateNonce(timestamp,nonce),AggregateNonce(timestamp_to_respond,nonce_to_respond),pto->GetId(),pfrom ? pfrom->GetId() : 0,msg_type,(int)vHops.size());
    pto->PushMessage("offchain",
                        msg_format,
                        vHops,
                        msg_type,
                        timestamp,
                        nonce,
                        timestamp_to_respond,
                        nonce_to_respond,
                        flags,
                        payload,
                        sigScripts);        
    
//    SetRelayRecord(pto,NULL,msg_type,timestamp,nonce);
    SetRelayRecord(pto,pfrom,msg_type,timestamp,nonce);
    
    return aggr_nonce;
}
    
bool mc_RelayManager::ProcessRelay( CNode* pfrom, 
                                    CDataStream& vRecv, 
                                    CValidationState &state, 
                                    uint32_t verify_flags_in)
{
    uint32_t  msg_type_in;
    uint32_t verify_flags;
    uint32_t   timestamp_received;
    uint32_t   nonce_received;
    uint32_t   timestamp_to_respond;
    uint32_t   nonce_to_respond;
    vector<unsigned char> vPayloadIn;
    vector<CScript> vSigScripts;
    vector<CScript> vSigScriptsEmpty;
    vector <int32_t> vHops;
    vector <int32_t> vEmptyHops;
    uint256   message_hash;
    uint32_t  flags_in,flags_response,flags_relay;
    vector<unsigned char> vchSigOut;
    vector<unsigned char> vchPubKey;
    CPubKey pubkey;
    vector<CAddress> path;    
    CNode *pto_stored;
    uint32_t msg_type_stored;
    uint32_t msg_format;
    unsigned char hop_count;
    uint32_t msg_type_response,msg_type_relay;
    uint32_t *msg_type_relay_ptr;
    uint32_t *msg_type_response_ptr;
    vector<unsigned char> vPayloadResponse;
    vector<unsigned char> vPayloadRelay;
    vector<CKeyID>  vAddrIn;    
    vector<CKeyID>  vAddrResponse;    
    vector<CKeyID>  vAddrRelay;    
    string strError;    
    
    msg_type_stored=MC_RMT_NONE;
    msg_type_response=MC_RMT_NONE;
    msg_type_relay=MC_RMT_NONE;
    pto_stored=NULL;
    
    verify_flags=verify_flags_in;
    vRecv >> msg_format;
    if(msg_format != 0)
    {
        LogPrintf("ProcessRelay() : Unsupported message format %08X\n",msg_format);     
        return false;        
    }
        
    vRecv >> vHops;
    hop_count=(int)vHops.size();
    vRecv >> msg_type_in;
    switch(msg_type_in)
    {
        case MC_RMT_MC_ADDRESS_QUERY:
            verify_flags |= MC_VRA_IS_NOT_RESPONSE;
            break;
        case MC_RMT_NODE_DETAILS:
            verify_flags |= MC_VRA_IS_RESPONSE | MC_VRA_SIGNATURE_ORIGIN;
            break;
        case MC_RMT_REJECT:
            verify_flags |= MC_VRA_IS_RESPONSE | MC_VRA_SIGNATURE_ORIGIN;
            break;
        case MC_RMT_CHUNK_QUERY:
            verify_flags |= MC_VRA_IS_NOT_RESPONSE;
            break;
        case MC_RMT_CHUNK_QUERY_HIT:
            verify_flags |= MC_VRA_IS_RESPONSE | MC_VRA_SIGNATURE_ORIGIN | MC_VRA_PROCESS_ONCE;
            break;            
        case MC_RMT_CHUNK_REQUEST:
            verify_flags |= MC_VRA_IS_RESPONSE | MC_VRA_DOUBLE_HOP | MC_VRA_SIGNATURE_ORIGIN;
            break;
        case MC_RMT_CHUNK_RESPONSE:
            verify_flags |= MC_VRA_IS_RESPONSE | MC_VRA_DOUBLE_HOP | MC_VRA_SIGNATURE_ORIGIN | MC_VRA_PROCESS_ONCE;
            break;
        default:
            if(verify_flags & MC_VRA_MESSAGE_TYPE)    
            {
                LogPrintf("ProcessRelay() : Unsupported relay message type %d\n",msg_type_in);     
                return false;
            }
            break;
    }
    
    if(verify_flags & MC_VRA_SINGLE_HOP)
    {
        if(hop_count)
        {
            LogPrintf("ProcessRelay() : Unsupported hop count %d for msg type %d\n",hop_count,msg_type_in);     
            return false;            
        }
    }
    
    if(verify_flags & MC_VRA_DOUBLE_HOP)
    {
        if(hop_count > 1)
        {
            LogPrintf("ProcessRelay() : Unsupported hop count %d for msg type %d\n",hop_count,msg_type_in);     
            return false;            
        }
    }
        
    CheckTime();
        
    vRecv >> timestamp_received;
    
    if(verify_flags & MC_VRA_TIMESTAMP)
    {
        if(timestamp_received+m_MinTimeShift < m_LastTime)
        {
            LogPrintf("ProcessRelay() : Timestamp too far in the past: %d\n",timestamp_received);     
            return false;                        
        }
        if(timestamp_received > m_LastTime + m_MaxTimeShift)
        {
            LogPrintf("ProcessRelay() : Timestamp too far in the future: %d\n",timestamp_received);     
            return false;                        
        }
    }    
    
    vRecv >> nonce_received;    
    
    
    msg_type_relay_ptr=&msg_type_relay;
    msg_type_response_ptr=&msg_type_response;
    
    if( verify_flags & (MC_VRA_PROCESS_ONCE | MC_VRA_BROADCAST_ONCE))
    {
        switch(GetRelayRecord(NULL,timestamp_received,nonce_received,NULL,NULL))
        {
            case MC_ERR_NOERROR:
                if(verify_flags & MC_VRA_PROCESS_ONCE)
                {
                    return false;
                }
                if(verify_flags & MC_VRA_BROADCAST_ONCE)
                {
                    msg_type_relay_ptr=NULL;
                }
                break;
            case MC_ERR_ERROR_IN_SCRIPT:                                        // We already processed this message, it has errors
                return false;                                        
            case MC_ERR_NOT_ALLOWED:
                LogPrintf("ProcessRelay() : Processing this message is not allowed by current limits or requesting peer was disconnected\n");     
                return false;                                        
        }
    }
    
    if(verify_flags & MC_VRA_DOUBLE_HOP)
    {
        if(hop_count)
        {
            msg_type_relay_ptr=NULL;
        }
    }
    
        
    vRecv >> timestamp_to_respond;
    vRecv >> nonce_to_respond;
    
    if( verify_flags & MC_VRA_IS_NOT_RESPONSE ) 
    {
        if( (timestamp_to_respond != 0) || (nonce_to_respond != 0) )
        {
            return state.DoS(100, error("ProcessRelay() : This message should not be response"),REJECT_INVALID, "bad-nonce");                
        }
    }
    
    int64_t aggr_nonce=AggregateNonce(timestamp_to_respond,nonce_to_respond);
    
    if( verify_flags & MC_VRA_IS_RESPONSE ) 
    {
        if( timestamp_to_respond == 0 )
        {
            return state.DoS(100, error("ProcessRelay() : This message should be response"),REJECT_INVALID, "bad-nonce");                
        }
        
        if(GetRelayRecord(pfrom,timestamp_to_respond,nonce_to_respond,&msg_type_stored,&pto_stored))
        {
            LogPrintf("ProcessRelay() : Response without request from peer %d\n",pfrom->GetId());     
            return false;
        }
    }
    
    SetRelayRecord(NULL,pfrom,MC_RMT_ERROR_IN_MESSAGE,timestamp_received,nonce_received);
    
    if(timestamp_to_respond)
    {
        if(pto_stored)
        {
            msg_type_response_ptr=NULL;        
        }
        else
        {
            msg_type_relay_ptr=NULL;
        }
    }
    
    map<uint32_t, mc_Limiter>::iterator itlim_all = m_Limiters.find(MC_RMT_NONE);
    map<uint32_t, mc_Limiter>::iterator itlim_msg = m_Limiters.find(msg_type_in);
    
    if(itlim_all != m_Limiters.end())
    {
        itlim_all->second.SetEvent(1,vRecv.size());
        if( verify_flags & MC_VRA_LIMIT_ALL ) 
        {
            if(itlim_all->second.Disallowed(m_LastTime))
            {
                return false;
            }
        }        
    }
    
    if(itlim_msg != m_Limiters.end())
    {
        itlim_msg->second.SetEvent(1,vRecv.size());
        if( verify_flags & MC_VRA_LIMIT_MSG_TYPE ) 
        {
            if(itlim_msg->second.Disallowed(m_LastTime))
            {
                return false;
            }
        }        
    }
    
    vRecv >> flags_in;
    vRecv >> vPayloadIn;
    vRecv >> vSigScripts;
            
//    printf("recv: %d, from: %d, to: %d, hc: %d, size: %d, ts: %u, nc: %u\n",msg_type_in,pfrom->GetId(),pto_stored ? pto_stored->GetId() : 0,hop_count,(int)vPayloadIn.size(),timestamp_received,nonce_received);
    if(fDebug)LogPrint("offchain","Offchain recv: %ld, request: %ld, from: %d, to: %d, msg: %d, hops: %d\n",
            AggregateNonce(timestamp_received,nonce_received),AggregateNonce(timestamp_to_respond,nonce_to_respond),pfrom->GetId(),pto_stored ? pto_stored->GetId() : 0,msg_type_in,hop_count);
    
    if( verify_flags & MC_VRA_SIGNATURES )
    {
        CHashWriter ssHash(SER_GETHASH, 0);
        ssHash << msg_type_in;        
        ssHash << timestamp_received;
        ssHash << nonce_received;
        ssHash << timestamp_to_respond;
        ssHash << nonce_to_respond;
        ssHash << flags_in;
        ssHash << vPayloadIn;
        
        message_hash=ssHash.GetHash();                
    
        if( verify_flags & MC_VRA_SIGNATURE_ORIGIN )
        {
            if(vSigScripts.size() < 1)
            {
                return state.DoS(100, error("ProcessRelay() : Missing sigScript"),REJECT_INVALID, "bad-sigscript");                            
            }
            if(vSigScripts[0].size())
            {
                CScript scriptSig=vSigScripts[0];

                opcodetype opcode;

                CScript::const_iterator pc = scriptSig.begin();

                if (!scriptSig.GetOp(pc, opcode, vchSigOut))
                {
                    return state.DoS(100, error("ProcessRelay() : Cannot extract signature from sigScript"),REJECT_INVALID, "bad-sigscript-signature");                
                }

                vchSigOut.resize(vchSigOut.size()-1);
                if (!scriptSig.GetOp(pc, opcode, vchPubKey))
                {
                    return state.DoS(100, error("ProcessRelay() : Cannot extract pubkey from sigScript"),REJECT_INVALID, "bad-sigscript-pubkey");                
                }

                CPubKey pubKeyOut(vchPubKey);
                if (!pubKeyOut.IsValid())
                {
                    return state.DoS(100, error("ProcessRelay() : Invalid pubkey"),REJECT_INVALID, "bad-sigscript-pubkey");                
                }        

                pubkey=pubKeyOut;

                CHashWriter ss(SER_GETHASH, 0);
                
                ss << vector<unsigned char>((unsigned char*)&message_hash, (unsigned char*)&message_hash+32);
                ss << vector<unsigned char>((unsigned char*)&aggr_nonce, (unsigned char*)&aggr_nonce+sizeof(aggr_nonce));
                uint256 signed_hash=ss.GetHash();

                if(!pubkey.Verify(signed_hash,vchSigOut))
                {
                    return state.DoS(100, error("ProcessRelay() : Wrong signature"),REJECT_INVALID, "bad-signature");                            
                }        
            }
            else
            {
                return state.DoS(100, error("ProcessRelay() : Empty sigScript"),REJECT_INVALID, "bad-sigscript");                
            }
        }
    }
    
    if(itlim_all != m_Limiters.end())
    {
        itlim_all->second.Increment();
    }
    
    if(itlim_msg != m_Limiters.end())
    {
        itlim_msg->second.Increment();        
    }    

    if(pto_stored)
    {
        PushRelay(pto_stored,msg_format,vHops,msg_type_in,timestamp_received,nonce_received,timestamp_to_respond,nonce_to_respond,flags_in,
                  vPayloadIn,vSigScripts,pfrom,MC_PRA_NONE);
    }
    else
    {
        if( (msg_type_relay_ptr != NULL) || (msg_type_response_ptr != NULL) )
        {
            if(MultichainRelayResponse(msg_type_stored,pto_stored,
                                       msg_type_in,flags_in,vPayloadIn,vAddrIn,
                                       msg_type_response_ptr,&flags_response,vPayloadResponse,vAddrResponse,
                                       msg_type_relay_ptr,&flags_relay,vPayloadRelay,vAddrRelay,strError))
            {
                if(msg_type_response_ptr && *msg_type_response_ptr)
                {
                    if(*msg_type_response_ptr != MC_RMT_ADD_RESPONSE)
                    {
                        PushRelay(pfrom,0,vEmptyHops,*msg_type_response_ptr,m_LastTime,0,timestamp_received,nonce_received,flags_response,
                                  vPayloadResponse,vSigScriptsEmpty,NULL,MC_PRA_GENERATE_NONCE);                    
                    }
                    else
                    {
                        map<int64_t, mc_RelayRequest>::iterator itreq = m_Requests.find(AggregateNonce(timestamp_to_respond,nonce_to_respond));
                        if(itreq != m_Requests.end())
                        {
                            Lock();
                            AddResponse(itreq->second.m_Nonce,pfrom,vHops.size() ? vHops[0] : 0,hop_count,AggregateNonce(timestamp_received,nonce_received),msg_type_in,flags_in,vPayloadIn,MC_RST_SUCCESS);
                            UnLock();
                        }                        
                        else
                        {
                            LogPrintf("ProcessRelay() : Response without stored request from peer %d\n",pfrom->GetId());     
                            return false;                            
                        }
                    }
                }
                if(msg_type_relay_ptr && *msg_type_relay_ptr)
                {
                    LOCK(cs_vNodes);
                    BOOST_FOREACH(CNode* pnode, vNodes)
                    {
                        if(pnode != pfrom)
                        {
                            PushRelay(pnode,msg_format,vHops,*msg_type_relay_ptr,timestamp_received,nonce_received,timestamp_to_respond,nonce_to_respond,flags_relay,
                                      vPayloadRelay,vSigScriptsEmpty,pfrom,MC_PRA_NONE);
                        }
                    }
                }                
            }
            else
            {
                LogPrintf("ProcessRelay() : Error processing request %08X from peer %d: %s\n",msg_type_in,pfrom->GetId(),strError.c_str());     
                return false;                
            }
        }        
    }
    
    if(timestamp_to_respond)
    {
        SetRelayRecord(NULL,pfrom,msg_type_in,timestamp_received,nonce_received);        
    }
    else
    {
        SetRelayRecord(NULL,pfrom,MC_RMT_NEW_REQUEST,timestamp_received,nonce_received);
    }
    
    return true;
}

/*
typedef struct mc_RelayResponse
{
    int64_t m_Nonce;
    uint32_t m_MsgType;
    uint32_t m_Flags;
    CNode *m_NodeFrom;
    int m_HopCount;
    mc_NodeFullAddress m_Source;
    uint32_t m_LastTryTimestamp;
    vector <unsigned char> m_Payload;
    vector <mc_RelayRequest> m_Requests;
    
    void Zero();
} mc_RelayResponse;

typedef struct mc_RelayRequest
{
    int64_t m_Nonce;
    uint32_t m_MsgType;
    uint32_t m_Flags;
    int64_t m_ParentNonce;
    int m_ParentResponseID;
    uint32_t m_LastTryTimestamp;
    int m_TryCount;
    uint32_t m_Status;
    vector <unsigned char> m_Payload;   
    vector <mc_RelayResponse> m_Responses;
    
    void Zero();
} mc_RelayRequest;
*/

void mc_RelayResponse::Zero()
{
    m_Nonce=0;
    m_MsgType=MC_RMT_NONE;
    m_Flags=0;
    m_NodeFrom=0;
    m_HopCount=0;
    m_TryCount=0;
    m_Source=0;
    m_LastTryTimestamp=0;
    m_Status=MC_RST_NONE;
    m_Payload.clear();
    m_Requests.clear();    
}

void mc_RelayRequest::Zero()
{
    m_Nonce=0;
    m_MsgType=MC_RMT_NONE;
    m_Flags=0;
    m_NodeTo=0;
    m_ParentNonce=0;
    m_ParentResponseID=-1;
    m_LastTryTimestamp=0;
    m_TryCount=0;
    m_Status=MC_RST_NONE;
    m_Payload.clear();   
    m_Responses.clear();
}

int mc_RelayManager::AddRequest(int64_t parent_nonce,int parent_response_id,CNode *pto,int64_t nonce,uint32_t msg_type,uint32_t flags,vector <unsigned char>& payload,uint32_t status)
{    
    int err=MC_ERR_NOERROR;
 
    Lock();
    map<int64_t, mc_RelayRequest>::iterator itreq_this = m_Requests.find(nonce);
    if(itreq_this == m_Requests.end())
    {    
        mc_RelayRequest request;

        request.m_Nonce=nonce;
        request.m_MsgType=msg_type;
        request.m_Flags=flags;
        request.m_NodeTo=pto ? pto->GetId() : 0;
        request.m_ParentNonce=parent_nonce;
        request.m_ParentResponseID=parent_response_id;
        request.m_LastTryTimestamp=0;
        request.m_TryCount=0;
        request.m_Status=status;
        request.m_Payload=payload;   
        request.m_Responses.clear();

        if(parent_nonce)
        {
            map<int64_t, mc_RelayRequest>::iterator itreq = m_Requests.find(parent_nonce);
            if(itreq != m_Requests.end())
            {
                if(parent_response_id < (int)(itreq->second.m_Responses.size()))
                {
                    itreq->second.m_Responses[parent_response_id].m_Requests.push_back(request);
                }
            }    
            else
            {
                err=MC_ERR_NOT_FOUND;            
            }
        }

//        printf("rqst: %lu, mt: %d, node: %d, size: %d, pr: %lu\n",nonce,msg_type,pto ? pto->GetId() : 0,(int)payload.size(),parent_nonce);
        if(fDebug)LogPrint("offchain","Offchain rqst: %ld, to: %d, msg: %d, size: %d\n",nonce,pto ? pto->GetId() : 0,msg_type,(int)payload.size());
        m_Requests.insert(make_pair(nonce,request));
    }
    else
    {
        err=MC_ERR_FOUND;
    }    
    
    UnLock();
    return err;            
}

int mc_RelayManager::AddResponse(int64_t request,CNode *pfrom,int32_t source,int hop_count,int64_t nonce,uint32_t msg_type,uint32_t flags,vector <unsigned char>& payload,uint32_t status)
{
//    printf("resp: %lu, mt: %d, node: %d, size: %d, rn: %lu\n",request,msg_type,pfrom ? pfrom->GetId() : 0,(int)payload.size(),nonce);
    if(fDebug)LogPrint("offchain","Offchain resp: %ld, request: %ld, from: %d, msg: %d, size: %d\n",nonce,request,pfrom ? pfrom->GetId() : 0,msg_type,(int)payload.size());
    if(request == 0)
    {
        return MC_ERR_NOERROR;
    }
    
    mc_RelayResponse response;
    response.m_Nonce=nonce;
    response.m_MsgType=msg_type;
    response.m_Flags=flags;
    response.m_NodeFrom=pfrom ? pfrom->GetId() : 0;
    response.m_HopCount=hop_count;
    response.m_Source=source;
    response.m_LastTryTimestamp=0;
    response.m_Status=status;
    response.m_Payload=payload;
    response.m_Requests.clear();    
    
    if(request)
    {
        map<int64_t, mc_RelayRequest>::iterator itreq = m_Requests.find(request);
        if(itreq != m_Requests.end())
        {    
            itreq->second.m_Responses.push_back(response);
            if(status & MC_RST_SUCCESS)
            {
                itreq->second.m_Status |= MC_RST_SUCCESS;
            }
        }
        else
        {
            return MC_ERR_NOT_FOUND;
        }
    }
    
    return MC_ERR_NOERROR; 
}

int mc_RelayManager::DeleteRequest(int64_t request)
{
    int err=MC_ERR_NOERROR;

    Lock();
    map<int64_t, mc_RelayRequest>::iterator itreq = m_Requests.find(request);
    if(itreq != m_Requests.end())
    {
        m_Requests.erase(itreq);       
    }    
    else
    {
        err= MC_ERR_NOT_FOUND;
    }
    
    UnLock();
    return err;     
}

mc_RelayRequest *mc_RelayManager::FindRequest(int64_t request)
{
    Lock();
    map<int64_t, mc_RelayRequest>::iterator itreq = m_Requests.find(request);
    if(itreq != m_Requests.end())
    {
        return &(itreq->second);
    }    
    
    UnLock();
    return NULL;    
}


int64_t mc_RelayManager::SendRequest(CNode* pto,uint32_t msg_type,uint32_t flags,vector <unsigned char>& payload)
{
    uint32_t timestamp=mc_TimeNowAsUInt();
    uint32_t nonce=GenerateNonce();
    int64_t aggr_nonce=AggregateNonce(timestamp,nonce);
    vector <int32_t> vEmptyHops;
    
    vector<CScript> vSigScriptsEmpty;

    {
        LOCK(cs_vNodes);
        BOOST_FOREACH(CNode* pnode, vNodes)
        {
            if( (pto == NULL) || (pnode == pto) )
            {
                PushRelay(pnode,0,vEmptyHops,msg_type,timestamp,nonce,0,0,flags,payload,vSigScriptsEmpty,NULL,0);                    
            }
        }
    }

    if(AddRequest(0,0,pto,aggr_nonce,msg_type,flags,payload,MC_RST_NONE) != MC_ERR_NOERROR)
    {
        return 0;
    }
    
    return aggr_nonce;
}

int64_t mc_RelayManager::SendNextRequest(mc_RelayResponse* response,uint32_t msg_type,uint32_t flags,vector <unsigned char>& payload)
{
    int64_t aggr_nonce;
    vector <int32_t> vEmptyHops;
    vector<CScript> vSigScriptsEmpty;

    aggr_nonce=0;
    {
        LOCK(cs_vNodes);
        BOOST_FOREACH(CNode* pnode, vNodes)
        {
            if( pnode->GetId() == response->m_NodeFrom )
            {
                aggr_nonce=PushRelay(pnode,0,vEmptyHops,msg_type,0,0,Timestamp(response->m_Nonce),Nonce(response->m_Nonce),
                                     flags,payload,vSigScriptsEmpty,NULL,MC_PRA_GENERATE_TIMESTAMP | MC_PRA_GENERATE_NONCE);                    
                if(AddRequest(0,0,pnode,aggr_nonce,msg_type,flags,payload,MC_RST_NONE) != MC_ERR_NOERROR)
                {
                    return 0;
                }
            }
        }
    }
    
    return aggr_nonce;
}


void mc_RelayManager::InvalidateResponsesFromDisconnected()
{
    LOCK(cs_vNodes);
    int m=(int)vNodes.size();
    Lock();
    BOOST_FOREACH(PAIRTYPE(const int64_t,mc_RelayRequest)& item, m_Requests)    
    {
        for(int i=0;i<(int)item.second.m_Responses.size();i++)
        {
            int n=0;
            while(n<m)
            {
                if(vNodes[n]->GetId() == item.second.m_Responses[i].m_NodeFrom)
                {
                    n=m+1;
                }
                n++;
            }
            if(n == m)
            {
                item.second.m_Responses[i].m_Status &= ~MC_RST_SUCCESS;
                item.second.m_Responses[i].m_Status |= MC_RST_DISCONNECTED;
            }
        }
    }    
    UnLock();
}
