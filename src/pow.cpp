// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2018 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <pow.h>

#include <arith_uint256.h>
#include <chain.h>
#include <primitives/block.h>
#include <uint256.h>

namespace {
    // returns a * exp(p/q) where |p/q| is small
    arith_uint256 mul_exp(arith_uint256 a, int64_t p, int64_t q)
    {
        bool isNegative = p < 0;
        uint64_t abs_p = p >= 0 ? p : -p;
        arith_uint256 result = a;
        uint64_t n = 0;
        while (a > 0) {
            ++n;
            a = a * abs_p / q / n;
            if (isNegative && (n % 2 == 1)) {
                result -= a;
            } else {
                result += a;
            }
        }
        return result;
    }
}

// ppcoin: find last block index up to pindex
const CBlockIndex* GetLastBlockIndex(const CBlockIndex* pindex, bool fProofOfStake)
{
    //CBlockIndex will be updated with information about the proof type later
    while (pindex && pindex->pprev && (pindex->IsProofOfStake() != fProofOfStake))
        pindex = pindex->pprev;
    return pindex;
}

inline arith_uint256 GetLimit(int nHeight, const Consensus::Params& params, bool fProofOfStake)
{
    if(fProofOfStake) {
        if(nHeight < params.QIP9Height) {
            return UintToArith256(params.posLimit);
        } else if(nHeight < params.nReduceBlocktimeHeight) {
            return UintToArith256(params.QIP9PosLimit);
        } else {
            return UintToArith256(params.RBTPosLimit);
        }
    } else {
        return UintToArith256(params.powLimit);
    }
}

unsigned int GetNextWorkRequired(const CBlockIndex* pindexLast, const CBlockHeader *pblock, const Consensus::Params& params, bool fProofOfStake)
{

    unsigned int  nTargetLimit = GetLimit(pindexLast ? pindexLast->nHeight+1 : 0, params, fProofOfStake).GetCompact();
    int nHeight = pindexLast->nHeight + 1;

    // genesis block
    if (pindexLast == NULL)
        return nTargetLimit;

    // first block
    const CBlockIndex* pindexPrev = GetLastBlockIndex(pindexLast, fProofOfStake);
    if (pindexPrev->pprev == NULL)
        return nTargetLimit;

    const CBlockIndex* pindexPrevPrev;
    if (nHeight < params.nReduceBlocktimeHeight + params.DifficultyAdjustmentInterval(nHeight)) {
        // second block
        pindexPrevPrev = GetLastBlockIndex(pindexPrev->pprev, fProofOfStake);
        if (pindexPrevPrev->pprev == NULL)
            return nTargetLimit;
    } else {
        int nHeightFirst = pindexLast->nHeight - params.DifficultyAdjustmentInterval(nHeight);
        if(nHeightFirst<0)
            return nTargetLimit;
        pindexPrevPrev = pindexLast->GetAncestor(nHeightFirst);
        if (pindexPrevPrev->pprev == NULL)
            return nTargetLimit;
    }

    // min difficulty
    if (params.fPowAllowMinDifficultyBlocks)
    {
        // Special difficulty rule for testnet:
        // If the new block's timestamp is more than 2* 10 minutes
        // then allow mining of a min-difficulty block.
        int nHeight = pindexLast->nHeight + 1;
        if (pblock->GetBlockTime() > pindexLast->GetBlockTime() + params.TargetSpacing(nHeight)*2)
            return nTargetLimit;
        else
        {
            // Return the last non-special-min-difficulty-rules-block
            const CBlockIndex* pindex = pindexLast;
            while (pindex->pprev && pindex->nHeight % params.DifficultyAdjustmentInterval(pindex->nHeight) != 0 && pindex->nBits == nTargetLimit)
                pindex = pindex->pprev;
            return pindex->nBits;
        }
        return pindexLast->nBits;
    }

    return CalculateNextWorkRequired(pindexPrev, pindexPrevPrev->GetBlockTime(), params, fProofOfStake);
}

double GetDifficultyFromNBits(uint32_t nBits)
{
    int nShift = (nBits >> 24) & 0xff;
    double dDiff =
        (double)0x0000ffff / (double)(nBits & 0x00ffffff);

    while (nShift < 29)
    {
        dDiff *= 256.0;
        nShift++;
    }
    while (nShift > 29)
    {
        dDiff /= 256.0;
        nShift--;
    }

    return dDiff;
}

unsigned int CalculateNextWorkRequired(const CBlockIndex* pindexLast, int64_t nFirstBlockTime, const Consensus::Params& params, bool fProofOfStake)
{
    if(fProofOfStake){
        if (params.fPoSNoRetargeting)
            return pindexLast->nBits;
    }else{
        if (params.fPowNoRetargeting)
            return pindexLast->nBits;
    }
    // Limit adjustment step
    int nHeight = pindexLast->nHeight + 1;
    int64_t nTargetSpacing = params.TargetSpacing(nHeight);
    int64_t nActualSpacing = pindexLast->GetBlockTime() - nFirstBlockTime;
    // Retarget
    const arith_uint256 bnTargetLimit = GetLimit(nHeight, params, fProofOfStake);
    // ppcoin: target change every block
    // ppcoin: retarget with exponential moving toward target spacing
    arith_uint256 bnNew;
    bnNew.SetCompact(pindexLast->nBits);
    int64_t nInterval = params.DifficultyAdjustmentInterval(nHeight); 

    if (nHeight < params.QIP9Height) {
        if (nActualSpacing < 0)
            nActualSpacing = nTargetSpacing;
        if (nActualSpacing > nTargetSpacing * 10)
            nActualSpacing = nTargetSpacing * 10;
        bnNew *= ((nInterval - 1) * nTargetSpacing + nActualSpacing + nActualSpacing);
        bnNew /= ((nInterval + 1) * nTargetSpacing);
    } else if (nHeight < params.nReduceBlocktimeHeight + nInterval) {
        if (nActualSpacing < 0)
            nActualSpacing = nTargetSpacing;
        if (nActualSpacing > nTargetSpacing * 20)
            nActualSpacing = nTargetSpacing * 20;
        uint32_t stakeTimestampMask=params.StakeTimestampMask(nHeight);
        bnNew = mul_exp(bnNew, 2 * (nActualSpacing - nTargetSpacing) / (stakeTimestampMask + 1), (nInterval + 1) * nTargetSpacing / (stakeTimestampMask + 1));
    } else {
        if((nHeight - params.nReduceBlocktimeHeight - 4) % 4 == 0){ //adjust every 4 blocks
            if (nActualSpacing < 0)
                nActualSpacing = params.nRBTPowTargetTimespan;
            if (nActualSpacing < params.nRBTPowTargetTimespan / 20)
                nActualSpacing = params.nRBTPowTargetTimespan / 20;
            if (nActualSpacing > params.nRBTPowTargetTimespan * 20)
                nActualSpacing = params.nRBTPowTargetTimespan * 20;

            arith_uint256 bnNewTmp, bnNewAvg, bnNewTotal;
            const CBlockIndex* pindex;

            pindex=pindexLast;

            for(int i=0;i<nInterval;i++){
                bnNewTmp.SetCompact(pindex->nBits);
                bnNewTotal+=bnNewTmp;
                pindex=pindex->pprev;
            }

            bnNewAvg=bnNewTotal/nInterval;
            uint32_t stakeTimestampMask=params.StakeTimestampMask(nHeight);

            bnNew = mul_exp(bnNewAvg, 4 * (nActualSpacing - params.nRBTPowTargetTimespan) / (stakeTimestampMask + 1), (nInterval + 1) * params.nRBTPowTargetTimespan / (stakeTimestampMask + 1));
        }
    }

    if (bnNew <= 0 || bnNew > bnTargetLimit)
        bnNew = bnTargetLimit;
    return bnNew.GetCompact();
}

bool CheckProofOfWork(uint256 hash, unsigned int nBits, const Consensus::Params& params, bool fProofOfStake)
{
    bool fNegative;
    bool fOverflow;
    arith_uint256 bnTarget;

    bnTarget.SetCompact(nBits, &fNegative, &fOverflow);

    // Check range
    if (fNegative || bnTarget == 0 || fOverflow || bnTarget > UintToArith256(params.powLimit))
        return false;

    // Check proof of work matches claimed amount
    if (UintToArith256(hash) > bnTarget)
        return false;

    return true;
}
