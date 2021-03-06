#include<stdio.h>
#include<stdlib.h>
#include<stdint.h>
#include "hash_index.h"
#include "load_unipath_size.h"
#include "binarys_qsort.h"

#define HASH_LEN 10000
#define REFLEN 0Xffffffff
// #define _DEBUG

hash_table** Table = NULL;
uint8_t hash_kmer = 8;
uni_seed*** uniseed2 = NULL;

void initHashTable(uint8_t h_k)
{
    int i;
    uint8_t r_i,r_ii;
    hash_kmer = h_k;
    bucket_num = 1 << (hash_kmer << 1);
    value_num = 50;  // for the same hash-key, we suppose there have at most 50 copies in the target, or we think it is repeat sequence
    // hit_num = readlen_max;
    hit_num = HASH_LEN;

    Table = (hash_table** )calloc(thread_n, sizeof(hash_table*));
    if (Table == NULL) 
    {
        fprintf(stderr, "memory allocate for hash table wrong!!!\n");
        exit(0);
    }
    for(r_i = 0;r_i < thread_n; ++r_i)
    {
        Table[r_i] = (hash_table* )calloc(2, sizeof(hash_table));

        for(r_ii = 0; r_ii < 2; ++r_ii)
        {
            Table[r_i][r_ii].bucket = (hash_entry* )malloc(bucket_num * sizeof(hash_entry));
            for (i = 0; i < bucket_num; ++i)
            {
                Table[r_i][r_ii].bucket[i].value_index = 0;
                Table[r_i][r_ii].bucket[i].value = (int* )calloc(value_num ,4);
            }
        }
    }

    uniseed2 = (uni_seed***)calloc(thread_n, sizeof(uni_seed**));
    for(r_i = 0;r_i < thread_n; ++r_i)
    {
        uniseed2[r_i] = (uni_seed** )calloc(2, sizeof(uni_seed*));
        for(r_ii = 0;r_ii < 2; ++r_ii)
        {
            uniseed2[r_i][r_ii] = (uni_seed* )calloc(hit_num , sizeof(uni_seed)); //the threshold should be change
        }
    }
    
}

int create_hash_table(uint8_t* target, uint32_t target_len, uint8_t r_i, uint8_t tid)
{
    int i, j;
    int len = target_len - hash_kmer;
    int key = 0;
    int key_part = 0; //the [1...hash_kmer-1] bit key
    int index = 0;
    hash_table *table = &Table[tid][r_i];
    
    //init the table
    for (i = 0; i < bucket_num; ++i)
    {
        table->bucket[i].value_index = 0;
    }

    //the first kmer i = 0
    for (j = 0; j < hash_kmer - 1; ++j)
    {
        key_part += target[hash_kmer - 1 - j]*(1 << (j << 1));
    }
    key = key_part + target[0]*(1 << ((hash_kmer - 1) << 1));
    index = table->bucket[key].value_index;
    table->bucket[key].value[index] = 0;//position in target of the kmer
    table->bucket[key].value_index += 1;
    //other kmers
    for (i = 1; i <= len; ++i)
    {
        key = (key_part << 2) + target[hash_kmer - 1 + i];
        // printf("key = %d \n", key);
        if (table->bucket[key].value_index >= value_num)
        {
            key_part = key - target[i]*(1 << ((hash_kmer - 1) << 1));
            continue;
        }
        table->bucket[key].value[table->bucket[key].value_index] = i; //kmer's position in target
        table->bucket[key].value_index += 1;//order of kmer appear
        // printf("value_index = %d\n", table->bucket[key].value_index);
        //cal the kmer's [1...hash_kmer-1] bit key as the next kmer's top hash_kmer-2 bit
        key_part = key - target[i]*(1 << ((hash_kmer - 1) << 1));
    }
    return 1;
}

void freeHashTable()
{
    int i;
    uint8_t r_i,r_ii;
    for(r_i = 0;r_i < thread_n; ++r_i)
    {
        for(r_ii = 0;r_ii < 2; ++r_ii)
        {
            for (i = 0; i < bucket_num; ++i)
            {
                if (Table[r_i][r_ii].bucket[i].value != NULL) free(Table[r_i][r_ii].bucket[i].value);
            }
            if(Table[r_i][r_ii].bucket != NULL)  free(Table[r_i][r_ii].bucket);
        }
        if(Table[r_i] != NULL) free(Table[r_i]);
    }
    if(Table != NULL) free(Table);

    for(r_i = 0;r_i < thread_n; ++r_i)
    {
        for(r_ii = 0;r_ii < 2; ++r_ii)
        {
            if (uniseed2[r_i][r_ii] != NULL) free(uniseed2[r_i][r_ii]);
        }
        if (uniseed2[r_i] != NULL) free(uniseed2[r_i]);
    }
    if (uniseed2 != NULL)    free(uniseed2);
}

static inline void get_refseq(uint8_t *ref, uint32_t len, uint32_t start)
{
    //check 
    uint32_t m;

    for (m = 0; m < len; ++m) 
    {
        ref[m] = (buffer_ref_seq[(m + start) >> 5] >> ((31 - ((m + start) & 0X1f)) << 1)) & 0X3;
    }
}

int hash_query(uint8_t* query, uint32_t query_len, uint32_t query_s, uint8_t r_i, uint32_t index, uint8_t tid)
{
    int i, m;
    int read_off;
    int key = 0;
    int key_part = 0;
    int value_idx;
    int hits = 0;
    int8_t seed_step = 1;
    int8_t j;
    hash_table *table = &Table[tid][r_i];

    uint8_t tmp_hash = hash_kmer;
    int hash_sub = 0;
    if (query_len < 50)
        tmp_hash = 6;

    // tmp_hash = 8;
    if (tmp_hash < hash_kmer)
    {
        hash_sub = (hash_kmer - tmp_hash) << 1;
    }

    // printf("****tmp_hash = %d*******\n", tmp_hash);
    int len = query_len - tmp_hash;
    int SCORE_THRE = tmp_hash + 5;
    //the first kmer
    for (i = 0; i < tmp_hash - seed_step; ++i)
    {
        key_part += query[tmp_hash - 1 - i]*(1 << (i << 1));
    }
    key = key_part;
    for (i = tmp_hash - seed_step; i < tmp_hash; ++i)
    {
        key += query[tmp_hash - 1 - i]*(1 << (i << 1));
    }

    int t_k;
    for (m = 0; m < (1<<hash_sub); ++m)
    {
        t_k = key + m;
        value_idx = table->bucket[t_k].value_index;
        // printf("value_idx = %d\n", value_idx);
        if (value_idx)
        {
            //found
            for (i = 0; i < value_idx; ++i)
            {
                uniseed2[tid][r_i][hits].seed_id = hits;
                uniseed2[tid][r_i][hits].ref_begin = query_s;
                uniseed2[tid][r_i][hits].ref_end = uniseed2[tid][r_i][hits].ref_begin + tmp_hash - 1;
                uniseed2[tid][r_i][hits].read_begin = table->bucket[t_k].value[i];
                uniseed2[tid][r_i][hits].read_end = uniseed2[tid][r_i][hits].read_begin + tmp_hash - 1;
                // fprintf(fp_tfu2, "%d\t%d\t%d\t%d\t%d\n", hits, uniseed2[r_i][hits].read_begin,uniseed2[r_i][hits].read_end,uniseed2[r_i][hits].ref_begin,uniseed2[r_i][hits].ref_end);
                hits++;
            }
        }
        //first kmer end
    }

    int break_d = 0;
    //other kmers
    for (read_off = seed_step; read_off <= len; read_off += seed_step)
    {
        key = (key_part << (seed_step << 1));
        for (i = 0; i < seed_step; ++i)
        {
            key += query[read_off + tmp_hash - 1 - i]*(1 << (i << 1));
        }

        for (m = 0; m < (1<<hash_sub); ++m)
        {
            t_k = key + m;
            value_idx = table->bucket[t_k].value_index;
            //query the key through hash table
            if (value_idx == 0) //can not found the kmer in hash table
            {
                continue;
            }
            if (hits + value_idx > hit_num)
            {
                break_d = 1;
                break;
            }
            for (i = 0; i < value_idx; ++i)
            {
                uniseed2[tid][r_i][hits].seed_id = hits;
                uniseed2[tid][r_i][hits].ref_begin = read_off + query_s;
                uniseed2[tid][r_i][hits].ref_end = uniseed2[tid][r_i][hits].ref_begin + tmp_hash - 1;
                uniseed2[tid][r_i][hits].read_begin = table->bucket[t_k].value[i];
                uniseed2[tid][r_i][hits].read_end = uniseed2[tid][r_i][hits].read_begin + tmp_hash - 1;
                
                // printf("uniseed[%d][%d].seed_id = %d\n",r_i,hits,uniseed[r_i][hits].seed_id);
                // printf("uniseed2[%d][%d].read_begin = %d\n",r_i,hits,uniseed2[r_i][hits].read_begin);
                // printf("uniseed2[%d][%d].read_end = %d\n",r_i,hits,uniseed2[r_i][hits].read_end);
                // printf("uniseed2[%d][%d].ref_begin = %d\n",r_i,hits,uniseed2[r_i][hits].ref_begin);
                // printf("uniseed2[%d][%d].ref_end = %d\n",r_i,hits,uniseed2[r_i][hits].ref_end);
                // fprintf(fp_tfu2, "%d\t%d\t%d\t%d\t%d\n", hits, uniseed2[r_i][hits].read_begin,uniseed2[r_i][hits].read_end,uniseed2[r_i][hits].ref_begin,uniseed2[r_i][hits].ref_end);

                hits++;
            }
        }
        if (break_d)
            break;
        //cal the next kmer key
        for (j = 0; j < seed_step; ++j)
        {
            key -= query[read_off + j]*(1 << ((tmp_hash - 1 - j) << 1));
        }
        key_part = key;
    }

    //sort by start position on read

    qsort(uniseed2[tid][r_i], hits, sizeof(uni_seed), compare_uniseed2);
    
    int qs;
    int qe;
    int ts;
    int te;
    int hits_new = 0;
    int length;

    if (hits == 0)
        return 0;

    int e1 = 0, s1 = 0;

    qs = uniseed2[tid][r_i][0].ref_begin;
    qe = uniseed2[tid][r_i][0].ref_end;
    ts = uniseed2[tid][r_i][0].read_begin;
    te = uniseed2[tid][r_i][0].read_end;
    for (i = 1; i < hits; ++i)
    {
        if((uniseed2[tid][r_i][i].read_begin > ts) && abs((uniseed2[tid][r_i][i].ref_begin - qs) - (uniseed2[tid][r_i][i].read_begin - ts)) < tmp_hash)   //hash_kmer as a param  for user defined
        {
            qe = uniseed2[tid][r_i][i].ref_end;
            te = uniseed2[tid][r_i][i].read_end;
            if (i == hits - 1)
            {
                e1 = i;
                if (e1 - s1 + 1 > 8)
                {
                    uniseed2[tid][r_i][hits_new].ref_begin = qs;
                    uniseed2[tid][r_i][hits_new].ref_end = qe;
                    uniseed2[tid][r_i][hits_new].read_begin = ts;
                    uniseed2[tid][r_i][hits_new].read_end = te;
                    hits_new++;
                }
            }
        }
        else
        {
            e1 = i - 1;

            if (e1 - s1 + 1 > 8)
            {
                uniseed2[tid][r_i][hits_new].ref_begin = qs;
                uniseed2[tid][r_i][hits_new].ref_end = qe;
                uniseed2[tid][r_i][hits_new].read_begin = ts;
                uniseed2[tid][r_i][hits_new].read_end = te;
                hits_new++;
            }

            if (i == hits - 1) break;
            s1 = e1 + 1;

            qs = uniseed2[tid][r_i][i].ref_begin;
            qe = uniseed2[tid][r_i][i].ref_end;
            ts = uniseed2[tid][r_i][i].read_begin;
            te = uniseed2[tid][r_i][i].read_end;
        }
    }

    for(i=0;i<hits_new;++i)
    {
        uniseed3[tid][r_i][index].seed_id = uniseed2[tid][r_i][i].seed_id;
        uniseed3[tid][r_i][index].read_begin = uniseed2[tid][r_i][i].read_begin;
        uniseed3[tid][r_i][index].read_end = uniseed2[tid][r_i][i].read_end;
        uniseed3[tid][r_i][index].ref_begin = uniseed2[tid][r_i][i].ref_begin;
        uniseed3[tid][r_i][index].ref_end = uniseed2[tid][r_i][i].ref_end;
        #ifdef _DEBUG
        fprintf(stdout, "%d\t%d\t%d\t%d\t%d\t%d\n", index, uniseed3[tid][r_i][index].read_begin,uniseed3[tid][r_i][index].read_end,uniseed3[tid][r_i][index].ref_begin,uniseed3[tid][r_i][index].ref_end,uniseed3[tid][r_i][index].read_end - uniseed3[tid][r_i][index].read_begin + 1);
        fprintf(fp_tfu2, "%d\t%d\t%d\t%d\t%d\t%d\n", index, uniseed3[tid][r_i][index].read_begin,uniseed3[tid][r_i][index].read_end,uniseed3[tid][r_i][index].ref_begin,uniseed3[tid][r_i][index].ref_end,uniseed3[tid][r_i][index].read_end - uniseed3[tid][r_i][index].read_begin + 1);
        #endif
        index++;
    }

    return hits_new;

}

void local_hash_process(uint8_t **target, uint32_t target_len, anchored_exons** anchored_exon, uint32_t* anchored_exon_num, uint32_t *uniseed2_length, uint8_t tid)
{
    uint8_t r_i;
    uint32_t i, index;
    uint32_t ae_s;
    uint32_t ae_len;
    uint8_t *query = NULL;
    int max_l = 0;
    int tmp_uni = 0;

    for(r_i = 0;r_i < 2; ++r_i)
    {
        for (i = 0; i < anchored_exon_num[r_i]; ++i)
        {
            int l = anchored_exon[r_i][i].ref_end - anchored_exon[r_i][i].ref_begin + 1;
            if (max_l < l)
                max_l = l;
        }
    }
    query = (uint8_t *)calloc(max_l, 1);

    for(r_i = 0;r_i < 2; ++r_i)
    {
	   create_hash_table(target[r_i], target_len, r_i, tid);
    }

    for(r_i = 0;r_i < 2; ++r_i)
    {  
        if (anchored_exon_num[r_i] == 0)
            continue;
        #ifdef _DEBUG
        fprintf(stderr, "re-seeding...output MB\n");
        fprintf(stderr, "seed_id\tread_begin\tread_end\tref_begin\tref_end\tcov\n");
        fprintf(stdout, "strand = %d\n", r_i);
        fprintf(fp_tfu2, "strand = %d\n", r_i);
        #endif
        index = 0;
        for(i = 0; i<anchored_exon_num[r_i]; ++i)
        {
            ae_s = anchored_exon[r_i][i].ref_begin;
            ae_len = anchored_exon[r_i][i].ref_end - anchored_exon[r_i][i].ref_begin + 1;
            get_refseq(query, ae_len, ae_s);
            #ifdef _DEBUG
            fprintf(fp_tfu2, "anchored_exon = %d\n", i);
            fprintf(stdout, "anchored_exon = %d\n", i);
            #endif
            tmp_uni = hash_query(query, ae_len, ae_s, r_i, index, tid);
            if (tmp_uni == 0)
            {
                anchored_exon[r_i][i].ref_begin = REFLEN;
                anchored_exon[r_i][i].ref_end = 0;
            }
            uniseed2_length[r_i] += tmp_uni;
            index = uniseed2_length[r_i];
        }
    }

    free(query);
}