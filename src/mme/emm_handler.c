#define TRACE_MODULE _emm_handler

#include "core_debug.h"
#include "core_lib.h"

#include "nas_message.h"

#include "mme_event.h"

#include "mme_kdf.h"
#include "nas_security.h"
#include "nas_conv.h"
#include "mme_s6a_handler.h"
#include "esm_build.h"
#include "emm_build.h"
#include "s1ap_build.h"
#include "s1ap_path.h"

void emm_handle_identity_request(mme_ue_t *ue);

void emm_handle_esm_message_container(
        mme_ue_t *ue, nas_esm_message_container_t *esm_message_container)
{
    pkbuf_t *esmbuf = NULL;
    event_t e;

    nas_esm_header_t *h = NULL;
    c_uint8_t pti = NAS_PROCEDURE_TRANSACTION_IDENTITY_UNASSIGNED;
    c_uint8_t ebi = NAS_EPS_BEARER_IDENTITY_UNASSIGNED;
    mme_bearer_t *bearer = NULL;

    d_assert(ue, return, "Null param");
    d_assert(esm_message_container, return, "Null param");
    d_assert(esm_message_container->len, return, "Null param");

    h = (nas_esm_header_t *)esm_message_container->data;
    d_assert(h, return, "Null param");

    pti = h->procedure_transaction_identity;
    ebi = h->eps_bearer_identity;
    if (pti == NAS_PROCEDURE_TRANSACTION_IDENTITY_UNASSIGNED && ebi)
        bearer = mme_bearer_find_by_ebi(ue, ebi);
    else if (ebi == NAS_EPS_BEARER_IDENTITY_UNASSIGNED && pti)
        bearer = mme_bearer_find_by_pti(ue, pti);

    if (!bearer)
        bearer = mme_bearer_add(ue, pti);
    d_assert(bearer, return, "Null param");

    /* The Packet Buffer(pkbuf_t) for NAS message MUST make a HEADROOM. 
     * When calculating AES_CMAC, we need to use the headroom of the packet. */
    esmbuf = pkbuf_alloc(NAS_HEADROOM, esm_message_container->len);
    d_assert(esmbuf, return, "Null param");
    memcpy(esmbuf->payload, 
            esm_message_container->data, esm_message_container->len);

    event_set(&e, MME_EVT_ESM_BEARER_MSG);
    event_set_param1(&e, (c_uintptr_t)bearer->index);
    event_set_param2(&e, (c_uintptr_t)esmbuf);
    mme_event_send(&e);
}

void emm_handle_attach_request(
        mme_ue_t *ue, nas_attach_request_t *attach_request)
{
    nas_eps_mobile_identity_t *eps_mobile_identity =
                    &attach_request->eps_mobile_identity;

    emm_handle_esm_message_container(
            ue, &attach_request->esm_message_container);

    /* Store UE specific information */
    memcpy(&ue->visited_plmn_id, &mme_self()->plmn_id, PLMN_ID_LEN);
    if (attach_request->presencemask &
        NAS_ATTACH_REQUEST_LAST_VISITED_REGISTERED_TAI_PRESENT)
    {
        nas_tracking_area_identity_t *last_visited_registered_tai = 
            &attach_request->last_visited_registered_tai;

        memcpy(&ue->visited_plmn_id, 
                &last_visited_registered_tai->plmn_id,
                PLMN_ID_LEN);
    }

    memcpy(&ue->ue_network_capability, 
            &attach_request->ue_network_capability,
            sizeof(attach_request->ue_network_capability));
    memcpy(&ue->ms_network_capability, 
            &attach_request->ms_network_capability,
            sizeof(attach_request->ms_network_capability));

    switch(eps_mobile_identity->imsi.type)
    {
        case NAS_EPS_MOBILE_IDENTITY_IMSI:
        {
            nas_imsi_to_bcd(
                &eps_mobile_identity->imsi, eps_mobile_identity->length,
                ue->imsi_bcd);
            core_bcd_to_buffer(ue->imsi_bcd, ue->imsi, &ue->imsi_len);
            d_assert(ue->imsi_len, return,
                    "Can't get IMSI(len:%d\n", ue->imsi_len);

            d_info("[NAS] Attach request : UE_IMSI[%s] --> EMM", 
                    ue->imsi_bcd);

            mme_s6a_send_air(ue);
            break;
        }
        case NAS_EPS_MOBILE_IDENTITY_GUTI:
        {
            nas_eps_mobile_identity_guti_t *guti = NULL;
            guti = &eps_mobile_identity->guti;

            d_info("[NAS] Attach request : UE_GUTI[G:%d,C:%d,M_TMSI:0x%x] --> EMM", 
                    guti->mme_gid,
                    guti->mme_code,
                    guti->m_tmsi);

            /* FIXME :Check if GUTI was assigend from us */
            
            /* FIXME :If not, forward the message to other MME */

            /* FIXME : Find UE based on GUTI.
             *         The record with GUTI,IMSI should be 
             *         stored in permanent DB
             */


            /* If not found,
               Initiate NAS Identity procedure to get UE IMSI */
            emm_handle_identity_request(ue);
            break;
        }
        default:
        {
            d_warn("Not implemented(type:%d)", 
                    eps_mobile_identity->imsi.type);
            
            return;
        }
    }
}

void emm_handle_identity_request(mme_ue_t *ue)
{
    status_t rv;
    mme_enb_t *enb = NULL;
    pkbuf_t *emmbuf = NULL, *s1apbuf = NULL;

    nas_message_t message;
    nas_identity_request_t *identity_request = 
        &message.emm.identity_request;

    d_assert(ue, return, "Null param");
    enb = ue->enb;
    d_assert(ue->enb, return, "Null param");

    memset(&message, 0, sizeof(message));
    message.emm.h.protocol_discriminator = NAS_PROTOCOL_DISCRIMINATOR_EMM;
    message.emm.h.message_type = NAS_IDENTITY_REQUEST;

    /* Request IMSI */
    identity_request->identity_type.type = NAS_IDENTITY_TYPE_2_IMSI;

    d_assert(nas_plain_encode(&emmbuf, &message) == CORE_OK && emmbuf,,);

    rv = s1ap_build_downlink_nas_transport(&s1apbuf, ue, emmbuf);
    d_assert(rv == CORE_OK && s1apbuf, 
            pkbuf_free(emmbuf); return, "s1ap build error");

    d_assert(s1ap_send_to_enb(enb, s1apbuf) == CORE_OK,, "s1ap send error");
}

void emm_handle_identity_response(
        mme_ue_t *ue, nas_identity_response_t *identity_response)
{
    nas_mobile_identity_t *mobile_identity = NULL;

    d_assert(ue, return, "Null param");
    d_assert(identity_response, return, "Null param");

    mobile_identity = &identity_response->mobile_identity;

    if (mobile_identity->imsi.type == NAS_IDENTITY_TYPE_2_IMSI)
    {
        nas_imsi_to_bcd(
            &mobile_identity->imsi, mobile_identity->length,
            ue->imsi_bcd);
        core_bcd_to_buffer(ue->imsi_bcd, ue->imsi, &ue->imsi_len);

        d_assert(ue->imsi_len, return,
                "Can't get IMSI(len:%d\n", ue->imsi_len);
    }
    else
    {
        d_warn("Not supported Identity type(%d)",mobile_identity->imsi.type);
        return;
    }

    /* Send Authentication Information Request to HSS */
    mme_s6a_send_air(ue);

}

void emm_handle_authentication_request(mme_ue_t *ue)
{
    status_t rv;
    mme_enb_t *enb = NULL;
    pkbuf_t *emmbuf = NULL, *s1apbuf = NULL;

    nas_message_t message;
    nas_authentication_request_t *authentication_request = 
        &message.emm.authentication_request;

    d_assert(ue, return, "Null param");
    enb = ue->enb;
    d_assert(ue->enb, return, "Null param");

    memset(&message, 0, sizeof(message));
    message.emm.h.protocol_discriminator = NAS_PROTOCOL_DISCRIMINATOR_EMM;
    message.emm.h.message_type = NAS_AUTHENTICATION_REQUEST;

    memcpy(authentication_request->authentication_parameter_rand.rand,
            ue->rand, RAND_LEN);
    memcpy(authentication_request->authentication_parameter_autn.autn,
            ue->autn, AUTN_LEN);
    authentication_request->authentication_parameter_autn.length = 
            AUTN_LEN;

    d_assert(nas_plain_encode(&emmbuf, &message) == CORE_OK && emmbuf,,);

    rv = s1ap_build_downlink_nas_transport(&s1apbuf, ue, emmbuf);
    d_assert(rv == CORE_OK && s1apbuf, 
            pkbuf_free(emmbuf); return, "s1ap build error");

    d_assert(s1ap_send_to_enb(enb, s1apbuf) == CORE_OK,, "s1ap send error");
}

void emm_handle_authentication_response(
        mme_ue_t *ue, nas_authentication_response_t *authentication_response)
{
    status_t rv;
    mme_enb_t *enb = NULL;
    pkbuf_t *emmbuf = NULL, *s1apbuf = NULL;

    nas_authentication_response_parameter_t *authentication_response_parameter =
        &authentication_response->authentication_response_parameter;

    nas_message_t message;
    nas_security_mode_command_t *security_mode_command = 
        &message.emm.security_mode_command;
    nas_security_algorithms_t *selected_nas_security_algorithms =
        &security_mode_command->selected_nas_security_algorithms;
    nas_key_set_identifier_t *nas_key_set_identifier =
        &security_mode_command->nas_key_set_identifier;
    nas_ue_security_capability_t *replayed_ue_security_capabilities = 
        &security_mode_command->replayed_ue_security_capabilities;

    d_assert(ue, return, "Null param");
    enb = ue->enb;
    d_assert(ue->enb, return, "Null param");

    if (authentication_response_parameter->length != ue->xres_len ||
        memcmp(authentication_response_parameter->res,
            ue->xres, ue->xres_len) != 0)
    {
        d_error("authentication failed");
        return;
    }

    d_info("[NAS] Authentication response : UE[%s] --> EMM", 
            ue->imsi_bcd);

    memset(&message, 0, sizeof(message));
    message.h.security_header_type = 
       NAS_SECURITY_HEADER_INTEGRITY_PROTECTED_AND_NEW_SECURITY_CONTEXT;
    message.h.protocol_discriminator = NAS_PROTOCOL_DISCRIMINATOR_EMM;

    message.emm.h.protocol_discriminator = NAS_PROTOCOL_DISCRIMINATOR_EMM;
    message.emm.h.message_type = NAS_SECURITY_MODE_COMMAND;

    selected_nas_security_algorithms->type_of_ciphering_algorithm =
        mme_self()->selected_enc_algorithm;
    selected_nas_security_algorithms->type_of_integrity_protection_algorithm =
        mme_self()->selected_int_algorithm;

    nas_key_set_identifier->tsc = 0;
    nas_key_set_identifier->nas_key_set_identifier = 0;

    replayed_ue_security_capabilities->length =
        sizeof(replayed_ue_security_capabilities->eea) +
        sizeof(replayed_ue_security_capabilities->eia) +
        sizeof(replayed_ue_security_capabilities->uea) +
        sizeof(replayed_ue_security_capabilities->uia) +
        sizeof(replayed_ue_security_capabilities->gea);
    replayed_ue_security_capabilities->eea = ue->ue_network_capability.eea;
    replayed_ue_security_capabilities->eia = ue->ue_network_capability.eia;
    replayed_ue_security_capabilities->uea = ue->ue_network_capability.uea;
    replayed_ue_security_capabilities->uia = ue->ue_network_capability.uia;
    replayed_ue_security_capabilities->gea = 
        (ue->ms_network_capability.gea1 << 6) | 
        ue->ms_network_capability.extended_gea;

    mme_kdf_nas(MME_KDF_NAS_INT_ALG, mme_self()->selected_int_algorithm,
            ue->kasme, ue->knas_int);
    mme_kdf_nas(MME_KDF_NAS_ENC_ALG, mme_self()->selected_enc_algorithm,
            ue->kasme, ue->knas_enc);
    mme_kdf_enb(ue->kasme, ue->ul_count.i32, ue->kenb);

    d_info("[NAS] Security mode command : UE[%s] <-- EMM", 
            ue->imsi_bcd);

    rv = nas_security_encode(&emmbuf, ue, &message);
    d_assert(rv == CORE_OK && emmbuf, return, "emm build error");

    rv = s1ap_build_downlink_nas_transport(&s1apbuf, ue, emmbuf);
    d_assert(rv == CORE_OK && s1apbuf, 
            pkbuf_free(emmbuf); return, "s1ap build error");

    d_assert(s1ap_send_to_enb(enb, s1apbuf) == CORE_OK,, "s1ap send error");
}

void emm_handle_create_session_response(mme_bearer_t *bearer)
{
    status_t rv;
    mme_ue_t *ue = NULL;
    mme_enb_t *enb = NULL;
    pkbuf_t *esmbuf = NULL, *emmbuf = NULL, *s1apbuf = NULL;

    d_assert(bearer, return, "Null param");
    ue = bearer->ue;
    d_assert(ue, return, "Null param");
    enb = ue->enb;
    d_assert(ue->enb, return, "Null param");

    rv = esm_build_activate_default_bearer_context(&esmbuf, bearer);
    d_assert(rv == CORE_OK && esmbuf, 
            return, "bearer build error");

    d_info("[NAS] Activate default bearer context request : EMM <-- ESM[%d]",
            bearer->ebi);

    rv = emm_build_attach_accept(&emmbuf, ue, esmbuf);
    d_assert(rv == CORE_OK && emmbuf, 
            pkbuf_free(esmbuf); return, "emm build error");

    d_info("[NAS] Attach accept : UE[%s] <-- EMM", ue->imsi_bcd);

    rv = s1ap_build_initial_context_setup_request(&s1apbuf, bearer, emmbuf);
    d_assert(rv == CORE_OK && s1apbuf, 
            pkbuf_free(emmbuf); return, "s1ap build error");

    d_assert(s1ap_send_to_enb(enb, s1apbuf) == CORE_OK,, "s1ap send error");
}

void emm_handle_attach_complete(
    mme_ue_t *ue, nas_attach_complete_t *attach_complete)
{
    status_t rv;
    mme_enb_t *enb = NULL;
    pkbuf_t *emmbuf = NULL, *s1apbuf = NULL;

    nas_message_t message;
    nas_emm_information_t *emm_information = &message.emm.emm_information;
    nas_time_zone_and_time_t *universal_time_and_local_time_zone =
        &emm_information->universal_time_and_local_time_zone;
    nas_daylight_saving_time_t *network_daylight_saving_time = 
        &emm_information->network_daylight_saving_time;

    time_exp_t time_exp;
    time_exp_lt(&time_exp, time_now());

    d_assert(ue, return, "Null param");
    enb = ue->enb;
    d_assert(enb, return, "Null param");

    emm_handle_esm_message_container(
            ue, &attach_complete->esm_message_container);

    memset(&message, 0, sizeof(message));
    message.h.security_header_type = 
       NAS_SECURITY_HEADER_INTEGRITY_PROTECTED_AND_CIPHERED;
    message.h.protocol_discriminator = NAS_PROTOCOL_DISCRIMINATOR_EMM;

    message.emm.h.protocol_discriminator = NAS_PROTOCOL_DISCRIMINATOR_EMM;
    message.emm.h.message_type = NAS_EMM_INFORMATION;

    emm_information->presencemask |=
        NAS_EMM_INFORMATION_UNIVERSAL_TIME_AND_LOCAL_TIME_ZONE_PRESENT;
    universal_time_and_local_time_zone->year = 
                NAS_TIME_TO_BCD(time_exp.tm_year % 100);
    universal_time_and_local_time_zone->mon = NAS_TIME_TO_BCD(time_exp.tm_mon);
    universal_time_and_local_time_zone->mday = 
                NAS_TIME_TO_BCD(time_exp.tm_mday);
    universal_time_and_local_time_zone->hour = 
                NAS_TIME_TO_BCD(time_exp.tm_hour);
    universal_time_and_local_time_zone->min = NAS_TIME_TO_BCD(time_exp.tm_min);
    universal_time_and_local_time_zone->sec = NAS_TIME_TO_BCD(time_exp.tm_sec);
    if (time_exp.tm_gmtoff > 0)
        universal_time_and_local_time_zone->sign = 0;
    else
        universal_time_and_local_time_zone->sign = 1;
    /* quarters of an hour */
    universal_time_and_local_time_zone->gmtoff = 
                NAS_TIME_TO_BCD(time_exp.tm_gmtoff / 900);

    emm_information->presencemask |=
        NAS_EMM_INFORMATION_NETWORK_DAYLIGHT_SAVING_TIME_PRESENT;
    network_daylight_saving_time->length = 1;

    d_info("[NAS] EMM information : UE[%s] <-- EMM", 
            ue->imsi_bcd);

    rv = nas_security_encode(&emmbuf, ue, &message);
    d_assert(rv == CORE_OK && emmbuf, return, "emm build error");

    rv = s1ap_build_downlink_nas_transport(&s1apbuf, ue, emmbuf);
    d_assert(rv == CORE_OK && s1apbuf, 
            pkbuf_free(emmbuf); return, "s1ap build error");

    d_assert(s1ap_send_to_enb(enb, s1apbuf) == CORE_OK,, "s1ap send error");
}

void emm_handle_emm_status(mme_ue_t *ue, nas_emm_status_t *emm_status)
{
    d_assert(ue, return, "Null param");

    d_warn("[NAS] EMM status(%d) : UE[%s] --> EMM", 
            emm_status->emm_cause, ue->imsi_bcd);
}

void emm_handle_detach_request(
        mme_ue_t *ue, nas_detach_request_from_ue_t *detach_request)
{
    status_t rv;
    mme_enb_t *enb = NULL;
    pkbuf_t *emmbuf = NULL, *s1apbuf = NULL;

    nas_message_t message;
    nas_detach_type_t *detach_type = &detach_request->detach_type;

    /* FIXME: nas_key_set_identifier is ignored
     * detach_type->tsc
     * detach_type->nas_key_set_identifier 
     */

    d_assert(ue, return, "Null param");
    enb = ue->enb;
    d_assert(enb, return, "Null param");

    switch (detach_type->detach_type)
    {
        /* 0 0 1 : EPS detach */
        case NAS_DETACH_TYPE_FROM_UE_EPS_DETACH: 
            break;
        /* 0 1 0 : IMSI detach */
        case NAS_DETACH_TYPE_FROM_UE_IMSI_DETACH: 
            break;
        case 6: /* 1 1 0 : reserved */
        case 7: /* 1 1 1 : reserved */
            break;
        /* 0 1 1 : combined EPS/IMSI detach */
        case NAS_DETACH_TYPE_FROM_UE_COMBINED_EPS_IMSI_DETACH: 
        default: /* all other values */
            break;
    }
    
    /* TODO: ESM session delete */

    if ((detach_type->switch_off & 0x1) == 0)
    {
        /* reply with detach accept */
        memset(&message, 0, sizeof(message));
        message.h.security_header_type = 
            NAS_SECURITY_HEADER_INTEGRITY_PROTECTED_AND_CIPHERED;
        message.h.protocol_discriminator = NAS_PROTOCOL_DISCRIMINATOR_EMM;

        message.emm.h.protocol_discriminator = NAS_PROTOCOL_DISCRIMINATOR_EMM;
        message.emm.h.message_type = NAS_DETACH_ACCEPT;

        d_info("[NAS] Detach accept : UE[%s] <-- EMM", 
            ue->imsi_bcd);

        rv = nas_security_encode(&emmbuf, ue, &message);
        d_assert(rv == CORE_OK && emmbuf, return, "emm build error");

        rv = s1ap_build_downlink_nas_transport(&s1apbuf, ue, emmbuf);
        d_assert(rv == CORE_OK && s1apbuf, 
            pkbuf_free(emmbuf); return, "s1ap build error");

        d_assert(s1ap_send_to_enb(enb, s1apbuf) == CORE_OK,, "s1ap send error");
    }

    /* initiate s1 ue context release */
}
