//------------------------------------------------------------------------------
/*
    This file is part of rippled: https://github.com/ripple/rippled
    Copyright (c) 2012, 2013 Ripple Labs Inc.

    Permission to use, copy, modify, and/or distribute this software for any
    purpose  with  or without fee is hereby granted, provided that the above
    copyright notice and this permission notice appear in all copies.

    THE  SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
    WITH  REGARD  TO  THIS  SOFTWARE  INCLUDING  ALL  IMPLIED  WARRANTIES  OF
    MERCHANTABILITY  AND  FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
    ANY  SPECIAL ,  DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
    WHATSOEVER  RESULTING  FROM  LOSS  OF USE, DATA OR PROFITS, WHETHER IN AN
    ACTION  OF  CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
    OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
*/
//==============================================================================

#include <ripple/app/tx/impl/PayChan.h>
#include <ripple/basics/Log.h>
#include <ripple/basics/XRPAmount.h>
#include <ripple/basics/chrono.h>
#include <ripple/ledger/ApplyView.h>
#include <ripple/ledger/View.h>
#include <ripple/protocol/Feature.h>
#include <ripple/protocol/Indexes.h>
#include <ripple/protocol/PayChan.h>
#include <ripple/protocol/PublicKey.h>
#include <ripple/protocol/TxFlags.h>
#include <ripple/protocol/digest.h>
#include <ripple/protocol/st.h>

namespace ripple {

/*
    PaymentChannel

        Payment channels permit off-ledger checkpoints of XRP payments flowing
        in a single direction. A channel sequesters the owner's XRP in its own
        ledger entry. The owner can authorize the recipient to claim up to a
        given balance by giving the receiver a signed message (off-ledger). The
        recipient can use this signed message to claim any unpaid balance while
        the channel remains open. The owner can top off the line as needed. If
        the channel has not paid out all its funds, the owner must wait out a
        delay to close the channel to give the recipient a chance to supply any
        claims. The recipient can close the channel at any time. Any transaction
        that touches the channel after the expiration time will close the
        channel. The total amount paid increases monotonically as newer claims
        are issued. When the channel is closed any remaining balance is returned
        to the owner. Channels are intended to permit intermittent off-ledger
        settlement of ILP trust lines as balances get substantial. For
        bidirectional channels, a payment channel can be used in each direction.

    PaymentChannelCreate

        Create a unidirectional channel. The parameters are:
        Destination
            The recipient at the end of the channel.
        Amount
            The amount of XRP to deposit in the channel immediately.
        SettleDelay
            The amount of time everyone but the recipient must wait for a
            superior claim.
        PublicKey
            The key that will sign claims against the channel.
        CancelAfter (optional)
            Any channel transaction that touches this channel after the
            `CancelAfter` time will close it.
        DestinationTag (optional)
            Destination tags allow the different accounts inside of a Hosted
            Wallet to be mapped back onto the Ripple ledger. The destination tag
            tells the server to which account in the Hosted Wallet the funds are
            intended to go to. Required if the destination has lsfRequireDestTag
            set.
        SourceTag (optional)
            Source tags allow the different accounts inside of a Hosted Wallet
            to be mapped back onto the Ripple ledger. Source tags are similar to
            destination tags but are for the channel owner to identify their own
            transactions.

    PaymentChannelFund

        Add additional funds to the payment channel. Only the channel owner may
        use this transaction. The parameters are:
        Channel
            The 256-bit ID of the channel.
        Amount
            The amount of XRP to add.
        Expiration (optional)
            Time the channel closes. The transaction will fail if the expiration
            times does not satisfy the SettleDelay constraints.

    PaymentChannelClaim

        Place a claim against an existing channel. The parameters are:
        Channel
            The 256-bit ID of the channel.
        Balance (optional)
            The total amount of XRP delivered after this claim is processed
   (optional, not needed if just closing). Amount (optional) The amount of XRP
   the signature is for (not needed if equal to Balance or just closing the
   line). Signature (optional) Authorization for the balance above, signed by
   the owner (optional, not needed if closing or owner is performing the
   transaction). The signature if for the following message: CLM\0 followed by
   the 256-bit channel ID, and a 64-bit integer drops. PublicKey (optional) The
   public key that made the signature (optional, required if a signature is
   present) Flags tfClose Request that the channel be closed tfRenew Request
   that the channel's expiration be reset. Only the owner may renew a channel.

*/

//------------------------------------------------------------------------------

static TER
closeChannel(
    std::shared_ptr<SLE> const& slep,
    ApplyView& view,
    uint256 const& key,
    beast::Journal j)
{
    AccountID const src = (*slep)[sfAccount];
    auto const amount = (*slep)[sfAmount] - (*slep)[sfBalance];

    std::shared_ptr<SLE> sleLine;

    if (!isXRP(amount))
    {
        if (!view.rules().enabled(featurePaychanAndEscrowForTokens))
            return tefINTERNAL;

        sleLine =
            view.peek(keylet::line(src, amount.getIssuer(), amount.getCurrency()));
    
        // dry run
        TER result = 
            trustAdjustLockedBalance(
                view,
                sleLine,
                -amount,
                -1,
                j,
                DryRun);

        JLOG(j.trace())
            << "closeChannel: trustAdjustLockedBalance(dry) result="
            << result;

        if (!isTesSuccess(result))
            return result;
    }

    // Remove PayChan from owner directory
    {
        auto const page = (*slep)[sfOwnerNode];
        if (!view.dirRemove(keylet::ownerDir(src), page, key, true))
        {
            JLOG(j.fatal())
                << "Could not remove paychan from src owner directory";
            return tefBAD_LEDGER;
        }
    }

    // Remove PayChan from recipient's owner directory, if present.
    if (auto const page = (*slep)[~sfDestinationNode];
        page && view.rules().enabled(fixPayChanRecipientOwnerDir))
    {
        auto const dst = (*slep)[sfDestination];
        if (!view.dirRemove(keylet::ownerDir(dst), *page, key, true))
        {
            JLOG(j.fatal())
                << "Could not remove paychan from dst owner directory";
            return tefBAD_LEDGER;
        }
    }

    // Transfer amount back to owner, decrement owner count
    auto const sle = view.peek(keylet::account(src));
    if (!sle)
        return tefINTERNAL;

    assert((*slep)[sfAmount] >= (*slep)[sfBalance]);

    if (isXRP(amount))
        (*sle)[sfBalance] = (*sle)[sfBalance] + amount;
    else
    {
        TER result = 
            trustAdjustLockedBalance(
                view,
                sleLine,
                -amount,
                -1,
                j,
                WetRun);

        JLOG(j.trace())
            << "closeChannel: trustAdjustLockedBalance(wet) result="
            << result;

        if (!isTesSuccess(result))
            return result;
    }

    adjustOwnerCount(view, sle, -1, j);
    view.update(sle);

    // Remove PayChan from ledger
    view.erase(slep);
    return tesSUCCESS;
}

//------------------------------------------------------------------------------

TxConsequences
PayChanCreate::makeTxConsequences(PreflightContext const& ctx)
{
    return TxConsequences{ctx.tx,
        isXRP(ctx.tx[sfAmount]) ? ctx.tx[sfAmount].xrp() : beast::zero};
}

NotTEC
PayChanCreate::preflight(PreflightContext const& ctx)
{
    if (ctx.rules.enabled(fix1543) && ctx.tx.getFlags() & tfUniversalMask)
        return temINVALID_FLAG;

    if (auto const ret = preflight1(ctx); !isTesSuccess(ret))
        return ret;

    STAmount const amount {ctx.tx[sfAmount]};
    if (!isXRP(amount))
    {
        if (!ctx.rules.enabled(featurePaychanAndEscrowForTokens))
            return temBAD_AMOUNT;

        if (!isLegalNet(amount))
            return temBAD_AMOUNT;

        if (isFakeXRP(amount))
            return temBAD_CURRENCY;

        if (ctx.tx[sfAccount] == amount.getIssuer())
        {
            JLOG(ctx.j.trace())
                << "Malformed transaction: Cannot paychan own tokens to self.";
            return temDST_IS_SRC;
        }
    }

    if (ctx.tx[sfAmount] <= beast::zero)
        return temBAD_AMOUNT;

    if (ctx.tx[sfAccount] == ctx.tx[sfDestination])
        return temDST_IS_SRC;

    if (!publicKeyType(ctx.tx[sfPublicKey]))
        return temMALFORMED;

    return preflight2(ctx);
}

TER
PayChanCreate::preclaim(PreclaimContext const& ctx)
{
    auto const account = ctx.tx[sfAccount];
    auto const sle = ctx.view.read(keylet::account(account));
    if (!sle)
        return terNO_ACCOUNT;

    STAmount const amount {ctx.tx[sfAmount]};

    auto const balance = (*sle)[sfBalance];
    auto const reserve =
        ctx.view.fees().accountReserve((*sle)[sfOwnerCount] + 1);

    if (balance < reserve)
        return tecINSUFFICIENT_RESERVE;

    auto const dst = ctx.tx[sfDestination];

    // Check reserve and funds availability
    if (isXRP(amount) && balance < reserve + ctx.tx[sfAmount])
        return tecUNFUNDED;
    else
    {
        if (!ctx.view.rules().enabled(featurePaychanAndEscrowForTokens))
            return tecINTERNAL;

        // check for any possible bars to a channel existing
        // between these accounts for this asset
        {
            TER result = 
                trustTransferAllowed(
                    ctx.view,
                    {account, dst},
                    amount.issue(),
                    ctx.j);
            JLOG(ctx.j.trace())
                << "PayChanCreate::preclaim trustTransferAllowed result="
                << result;

            if (!isTesSuccess(result))
                return result;
        }

        // check if the amount can be locked
        {
            auto sleLine = 
                ctx.view.read(
                    keylet::line(account, amount.getIssuer(), amount.getCurrency()));
            TER result = 
                trustAdjustLockedBalance(
                    ctx.view,
                    sleLine,
                    amount,
                    1,
                    ctx.j,
                    DryRun);
            
            JLOG(ctx.j.trace())
                << "PayChanCreate::preclaim trustAdjustLockedBalance(dry) result="
                << result;

            if (!isTesSuccess(result))
                return result;
        }
    }

    {
        // Check destination account
        auto const sled = ctx.view.read(keylet::account(dst));
        if (!sled)
            return tecNO_DST;
        if (((*sled)[sfFlags] & lsfRequireDestTag) &&
            !ctx.tx[~sfDestinationTag])
            return tecDST_TAG_NEEDED;

        // Obeying the lsfDisallowXRP flag was a bug.  Piggyback on
        // featureDepositAuth to remove the bug.
        if (!ctx.view.rules().enabled(featureDepositAuth) &&
            ((*sled)[sfFlags] & lsfDisallowXRP))
            return tecNO_TARGET;
    }

    return tesSUCCESS;
}

TER
PayChanCreate::doApply()
{
    auto const account = ctx_.tx[sfAccount];
    auto const sle = ctx_.view().peek(keylet::account(account));
    if (!sle)
        return tefINTERNAL;

    auto const dst = ctx_.tx[sfDestination];

    STAmount const amount {ctx_.tx[sfAmount]};

    // Create PayChan in ledger.
    //
    // Note that we we use the value from the sequence or ticket as the
    // payChan sequence.  For more explanation see comments in SeqProxy.h.
    Keylet const payChanKeylet =
        keylet::payChan(account, dst, ctx_.tx.getSeqProxy().value());
    auto const slep = std::make_shared<SLE>(payChanKeylet);

    // Funds held in this channel
    (*slep)[sfAmount] = ctx_.tx[sfAmount];
    // Amount channel has already paid
    (*slep)[sfBalance] = ctx_.tx[sfAmount].zeroed();
    (*slep)[sfAccount] = account;
    (*slep)[sfDestination] = dst;
    (*slep)[sfSettleDelay] = ctx_.tx[sfSettleDelay];
    (*slep)[sfPublicKey] = ctx_.tx[sfPublicKey];
    (*slep)[~sfCancelAfter] = ctx_.tx[~sfCancelAfter];
    (*slep)[~sfSourceTag] = ctx_.tx[~sfSourceTag];
    (*slep)[~sfDestinationTag] = ctx_.tx[~sfDestinationTag];

    ctx_.view().insert(slep);

    // Add PayChan to owner directory
    {
        auto const page = ctx_.view().dirInsert(
            keylet::ownerDir(account),
            payChanKeylet,
            describeOwnerDir(account));
        if (!page)
            return tecDIR_FULL;
        (*slep)[sfOwnerNode] = *page;
    }

    // Add PayChan to the recipient's owner directory
    if (ctx_.view().rules().enabled(fixPayChanRecipientOwnerDir))
    {
        auto const page = ctx_.view().dirInsert(
            keylet::ownerDir(dst), payChanKeylet, describeOwnerDir(dst));
        if (!page)
            return tecDIR_FULL;
        (*slep)[sfDestinationNode] = *page;
    }

    // Deduct owner's balance, increment owner count
    if (isXRP(amount))
        (*sle)[sfBalance] = (*sle)[sfBalance] - amount;
    else
    {
        if (!ctx_.view().rules().enabled(featurePaychanAndEscrowForTokens))
            return tefINTERNAL;

        auto sleLine =
            ctx_.view().peek(keylet::line(account, amount.getIssuer(), amount.getCurrency()));

        if (!sleLine)
            return tecUNFUNDED_PAYMENT;

        TER result = 
            trustAdjustLockedBalance(
                ctx_.view(),
                sleLine,
                amount,
                1,
                ctx_.journal,
                WetRun);

        JLOG(ctx_.journal.trace())
            << "PayChanCreate::doApply trustAdjustLockedBalance(wet) result="
            << result;

        if (!isTesSuccess(result))
            return tefINTERNAL;
    }
    
    adjustOwnerCount(ctx_.view(), sle, 1, ctx_.journal);
    ctx_.view().update(sle);

    return tesSUCCESS;
}

//------------------------------------------------------------------------------

TxConsequences
PayChanFund::makeTxConsequences(PreflightContext const& ctx)
{
    return TxConsequences{ctx.tx,
        isXRP(ctx.tx[sfAmount]) ? ctx.tx[sfAmount].xrp() : beast::zero};
}

NotTEC
PayChanFund::preflight(PreflightContext const& ctx)
{
    if (ctx.rules.enabled(fix1543) && ctx.tx.getFlags() & tfUniversalMask)
        return temINVALID_FLAG;

    if (auto const ret = preflight1(ctx); !isTesSuccess(ret))
        return ret;

    STAmount const amount {ctx.tx[sfAmount]};
    if (!isXRP(amount))
    {
        if (!ctx.rules.enabled(featurePaychanAndEscrowForTokens))
            return temBAD_AMOUNT;

        if (!isLegalNet(amount))
            return temBAD_AMOUNT;

        if (isFakeXRP(amount))
            return temBAD_CURRENCY;

        if (ctx.tx[sfAccount] == amount.getIssuer())
        {
            JLOG(ctx.j.trace())
                << "Malformed transaction: Cannot escrow own tokens to self.";
            return temDST_IS_SRC;
        }
    }

    if (ctx.tx[sfAmount] <= beast::zero)
        return temBAD_AMOUNT;

    return preflight2(ctx);
}

TER
PayChanFund::doApply()
{
    Keylet const k(ltPAYCHAN, ctx_.tx[sfChannel]);
    auto const slep = ctx_.view().peek(k);
    if (!slep)
        return tecNO_ENTRY;
    
    STAmount const amount {ctx_.tx[sfAmount]};

    std::shared_ptr<SLE> sleLine;   // if XRP or featurePaychanAndEscrowForTokens
                                    // not enabled this remains null

    // if this is a Fund operation on an IOU then perform a dry run here
    if (!isXRP(amount) &&
            ctx_.view().rules().enabled(featurePaychanAndEscrowForTokens))
    {
        sleLine = ctx_.view().peek(
            keylet::line(
                (*slep)[sfAccount], 
                amount.getIssuer(),
                amount.getCurrency()));

        TER result =
            trustAdjustLockedBalance(
                ctx_.view(),
                sleLine,
                amount,
                1,
                ctx_.journal,
                DryRun);

        JLOG(ctx_.journal.trace())
            << "PayChanFund::doApply trustAdjustLockedBalance(dry) result="
            << result;

        if (!isTesSuccess(result))
            return result;
    }

    AccountID const src = (*slep)[sfAccount];
    auto const txAccount = ctx_.tx[sfAccount];
    auto const expiration = (*slep)[~sfExpiration];
    {
        auto const cancelAfter = (*slep)[~sfCancelAfter];
        auto const closeTime =
            ctx_.view().info().parentCloseTime.time_since_epoch().count();
        if ((cancelAfter && closeTime >= *cancelAfter) ||
            (expiration && closeTime >= *expiration))
            return closeChannel(
                slep, ctx_.view(), k.key, ctx_.app.journal("View"));
    }

    if (src != txAccount)
        // only the owner can add funds or extend
        return tecNO_PERMISSION;

    if (auto extend = ctx_.tx[~sfExpiration])
    {
        auto minExpiration =
            ctx_.view().info().parentCloseTime.time_since_epoch().count() +
            (*slep)[sfSettleDelay];
        if (expiration && *expiration < minExpiration)
            minExpiration = *expiration;

        if (*extend < minExpiration)
            return temBAD_EXPIRATION;
        (*slep)[~sfExpiration] = *extend;
        ctx_.view().update(slep);
    }

    auto const sle = ctx_.view().peek(keylet::account(txAccount));
    if (!sle)
        return tefINTERNAL;

    // do not allow adding funds if dst does not exist
    if (AccountID const dst = (*slep)[sfDestination];
        !ctx_.view().read(keylet::account(dst)))
    {
        return tecNO_DST;
    }

    // Check reserve and funds availability
    auto const balance = (*sle)[sfBalance];
    auto const reserve =
        ctx_.view().fees().accountReserve((*sle)[sfOwnerCount]);

    if (balance < reserve)
        return tecINSUFFICIENT_RESERVE;


    if (isXRP(amount))
    {
        if (balance < reserve + amount)
            return tecUNFUNDED;

        (*sle)[sfBalance] = (*sle)[sfBalance] - amount;
        ctx_.view().update(sle);
    }
    else
    {
        if (!ctx_.view().rules().enabled(featurePaychanAndEscrowForTokens))
            return tefINTERNAL;


        TER result =
            trustAdjustLockedBalance(
                ctx_.view(),
                sleLine,
                amount,
                1,
                ctx_.journal,
                WetRun);
        
        JLOG(ctx_.journal.trace())
            << "PayChanFund::doApply trustAdjustLockedBalance(wet) result="
            << result;

        if (!isTesSuccess(result))
            return tefINTERNAL;
    }

    (*slep)[sfAmount] = (*slep)[sfAmount] + ctx_.tx[sfAmount];
    ctx_.view().update(slep);

    return tesSUCCESS;
}

//------------------------------------------------------------------------------

NotTEC
PayChanClaim::preflight(PreflightContext const& ctx)
{
    if (auto const ret = preflight1(ctx); !isTesSuccess(ret))
        return ret;

    auto const bal = ctx.tx[~sfBalance];
    if (bal)
    {
        if (!isXRP(*bal) && !ctx.rules.enabled(featurePaychanAndEscrowForTokens))
            return temBAD_AMOUNT;

        if (*bal <= beast::zero)
            return temBAD_AMOUNT;
    }

    auto const amt = ctx.tx[~sfAmount];

    if (amt)
    {
        if (!isXRP(*amt) && !ctx.rules.enabled(featurePaychanAndEscrowForTokens))
            return temBAD_AMOUNT;

        if (*amt <= beast::zero)
            return temBAD_AMOUNT;
    }

    if (bal && amt && *bal > *amt)
        return temBAD_AMOUNT;

    {
        auto const flags = ctx.tx.getFlags();

        if (ctx.rules.enabled(fix1543) && (flags & tfPayChanClaimMask))
            return temINVALID_FLAG;

        if ((flags & tfClose) && (flags & tfRenew))
            return temMALFORMED;
    }

    if (auto const sig = ctx.tx[~sfSignature])
    {
        if (!(ctx.tx[~sfPublicKey] && bal))
            return temMALFORMED;

        // Check the signature
        // The signature isn't needed if txAccount == src, but if it's
        // present, check it

        auto const reqBalance = *bal;
        auto const authAmt = *amt ? *amt : reqBalance;

        if (reqBalance > authAmt)
            return temBAD_AMOUNT;

        Keylet const k(ltPAYCHAN, ctx.tx[sfChannel]);
        if (!publicKeyType(ctx.tx[sfPublicKey]))
            return temMALFORMED;

        PublicKey const pk(ctx.tx[sfPublicKey]);
        Serializer msg;

        if (isXRP(authAmt))
            serializePayChanAuthorization(msg, k.key, authAmt.xrp());
        else
            serializePayChanAuthorization(msg, k.key, authAmt.iou(), authAmt.getCurrency(), authAmt.getIssuer());

        if (!verify(pk, msg.slice(), *sig, /*canonical*/ true))
            return temBAD_SIGNATURE;
    }

    return preflight2(ctx);
}

TER
PayChanClaim::doApply()
{
    Keylet const k(ltPAYCHAN, ctx_.tx[sfChannel]);
    auto const slep = ctx_.view().peek(k);
    if (!slep)
        return tecNO_TARGET;

    AccountID const src = (*slep)[sfAccount];
    AccountID const dst = (*slep)[sfDestination];
    AccountID const txAccount = ctx_.tx[sfAccount];

    auto const curExpiration = (*slep)[~sfExpiration];
    {
        auto const cancelAfter = (*slep)[~sfCancelAfter];
        auto const closeTime =
            ctx_.view().info().parentCloseTime.time_since_epoch().count();
        if ((cancelAfter && closeTime >= *cancelAfter) ||
            (curExpiration && closeTime >= *curExpiration))
            return closeChannel(
                slep, ctx_.view(), k.key, ctx_.app.journal("View"));
    }

    if (txAccount != src && txAccount != dst)
        return tecNO_PERMISSION;

    if (ctx_.tx[~sfBalance])
    {
        auto const chanBalance = slep->getFieldAmount(sfBalance);
        auto const chanFunds = slep->getFieldAmount(sfAmount);
        auto const reqBalance = ctx_.tx[sfBalance];

        if (txAccount == dst && !ctx_.tx[~sfSignature])
            return temBAD_SIGNATURE;

        if (ctx_.tx[~sfSignature])
        {
            PublicKey const pk((*slep)[sfPublicKey]);
            if (ctx_.tx[sfPublicKey] != pk)
                return temBAD_SIGNER;
        }

        if (reqBalance > chanFunds)
            return tecUNFUNDED_PAYMENT;

        if (reqBalance <= chanBalance)
            // nothing requested
            return tecUNFUNDED_PAYMENT;

        auto sled = ctx_.view().peek(keylet::account(dst));
        if (!sled)
            return tecNO_DST;

        // Obeying the lsfDisallowXRP flag was a bug.  Piggyback on
        // featureDepositAuth to remove the bug.
        bool const depositAuth{ctx_.view().rules().enabled(featureDepositAuth)};
        if (!depositAuth &&
            // RH TODO: does this condition need to be changed for IOU paychans?
            (txAccount == src && (sled->getFlags() & lsfDisallowXRP)))
            return tecNO_TARGET;

        // Check whether the destination account requires deposit authorization.
        if (depositAuth && (sled->getFlags() & lsfDepositAuth))
        {
            // A destination account that requires authorization has two
            // ways to get a Payment Channel Claim into the account:
            //  1. If Account == Destination, or
            //  2. If Account is deposit preauthorized by destination.
            if (txAccount != dst)
            {
                if (!view().exists(keylet::depositPreauth(dst, txAccount)))
                    return tecNO_PERMISSION;
            }
        }

        (*slep)[sfBalance] = ctx_.tx[sfBalance];
        STAmount const reqDelta = reqBalance - chanBalance;
        assert(reqDelta >= beast::zero);
        if (isXRP(reqDelta))
            (*sled)[sfBalance] = (*sled)[sfBalance] + reqDelta;
        else 
        {
            // xfer locked tokens to satisfy claim
            // RH NOTE: there's no ledger modification before this point so
            // no reason to do a dry run first
            if (!ctx_.view().rules().enabled(featurePaychanAndEscrowForTokens))
                return tefINTERNAL;

            auto sleSrcAcc = ctx_.view().peek(keylet::account(src));
            TER result =
                trustTransferLockedBalance(
                    ctx_.view(),
                    txAccount,
                    sleSrcAcc,
                    sled,
                    reqDelta,
                    0,
                    ctx_.journal,
                    WetRun);
            
            JLOG(ctx_.journal.trace())
                << "PayChanClaim::doApply trustTransferLockedBalance(wet) result="
                << result;

            if (!isTesSuccess(result))
                return result;
        }

        ctx_.view().update(sled);
        ctx_.view().update(slep);
    }

    if (ctx_.tx.getFlags() & tfRenew)
    {
        if (src != txAccount)
            return tecNO_PERMISSION;
        (*slep)[~sfExpiration] = std::nullopt;
        ctx_.view().update(slep);
    }

    if (ctx_.tx.getFlags() & tfClose)
    {
        // Channel will close immediately if dry or the receiver closes
        if (dst == txAccount || (*slep)[sfBalance] == (*slep)[sfAmount])
            return closeChannel(
                slep, ctx_.view(), k.key, ctx_.app.journal("View"));

        auto const settleExpiration =
            ctx_.view().info().parentCloseTime.time_since_epoch().count() +
            (*slep)[sfSettleDelay];

        if (!curExpiration || *curExpiration > settleExpiration)
        {
            (*slep)[~sfExpiration] = settleExpiration;
            ctx_.view().update(slep);
        }
    }

    return tesSUCCESS;
}

}  // namespace ripple
