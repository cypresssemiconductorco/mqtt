/*
 * Copyright 2021, Cypress Semiconductor Corporation (an Infineon company) or
 * an affiliate of Cypress Semiconductor Corporation.  All rights reserved.
 *
 * This software, including source code, documentation and related
 * materials ("Software") is owned by Cypress Semiconductor Corporation
 * or one of its affiliates ("Cypress") and is protected by and subject to
 * worldwide patent protection (United States and foreign),
 * United States copyright laws and international treaty provisions.
 * Therefore, you may use this Software only as provided in the license
 * agreement accompanying the software package from which you
 * obtained this Software ("EULA").
 * If no EULA applies, Cypress hereby grants you a personal, non-exclusive,
 * non-transferable license to copy, modify, and compile the Software
 * source code solely for use in connection with Cypress's
 * integrated circuit products.  Any reproduction, modification, translation,
 * compilation, or representation of this Software except as specified
 * above is prohibited without the express written permission of Cypress.
 *
 * Disclaimer: THIS SOFTWARE IS PROVIDED AS-IS, WITH NO WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING, BUT NOT LIMITED TO, NONINFRINGEMENT, IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE. Cypress
 * reserves the right to make changes to the Software without notice. Cypress
 * does not assume any liability arising out of the application or use of the
 * Software or any product or circuit described in the Software. Cypress does
 * not authorize its products for use in any products where a malfunction or
 * failure of the Cypress product may reasonably be expected to result in
 * significant property damage, injury or death ("High Risk Product"). By
 * including Cypress's product in a High Risk Product, the manufacturer
 * of such system or application assumes all risk of such use and in doing
 * so agrees to indemnify Cypress against all liability.
 */

/** @file
 *  Implements MQTT wrapper APIs to perform MQTT CONNECT, DISCONNET, PUBLISH, SUBSCRIBE, and UNSUBSCRIBE.
 *
 */
#include <string.h>
#include <stdlib.h>
#include "cy_mqtt_api.h"
#include "cy_utils.h"
#include "cyabs_rtos.h"

/******************************************************
 *                      Macros
 ******************************************************/
#ifdef ENABLE_MQTT_LOGS
#define cy_mqtt_log_msg cy_log_msg
#else
#define cy_mqtt_log_msg(a,b,c,...)
#endif

/**
 * Timeout for receiving CONNACK packet in milliseconds.
 */
#define CY_MQTT_CONNACK_RECV_TIMEOUT_MS                      ( 2000U )

/**
 * Network socket receive timeout in milliseconds.
 */
#define CY_MQTT_SOCKET_RECEIVE_TIMEOUT_MS                    ( 1U )

/**
 * Timeout in milliseconds for ProcessLoop.
 */
#define CY_MQTT_RECEIVE_DATA_TIMEOUT_MS                      ( 0U )

/**
 * Receive thread sleep time in milliseconds.
 */
#define CY_MQTT_RECEIVE_THREAD_SLEEP_MS                      ( 100U )

#ifndef CY_MQTT_RECEIVE_THREAD_STACK_SIZE
    #ifdef ENABLE_MQTT_LOGS
        #define CY_MQTT_RECEIVE_THREAD_STACK_SIZE            ( (1024 * 2) + (1024 * 3) ) /* Additional 3kb of stack is added for enabling the prints */
    #else
        #define CY_MQTT_RECEIVE_THREAD_STACK_SIZE            ( 1024 * 2 )
    #endif
#else
    #ifdef ENABLE_MQTT_LOGS
        #define CY_MQTT_RECEIVE_THREAD_STACK_SIZE_USER_DEBUG ( (CY_MQTT_RECEIVE_THREAD_STACK_SIZE) + (1024 * 3) ) /* Additional 3kb of stack is added for enabling the prints */
        #undef  CY_MQTT_RECEIVE_THREAD_STACK_SIZE
        #define CY_MQTT_RECEIVE_THREAD_STACK_SIZE            ( CY_MQTT_RECEIVE_THREAD_STACK_SIZE_USER_DEBUG )
    #endif
#endif

#define CY_MQTT_RECEIVE_THREAD_PRIORITY                      ( CY_RTOS_PRIORITY_NORMAL )

#define CY_MQTT_DISCONNECT_EVENT_QUEUE_SIZE                  ( CY_MQTT_MAX_HANDLE )

#ifdef ENABLE_MQTT_LOGS
    #define CY_MQTT_DISCONNECT_EVENT_THREAD_STACK_SIZE       ( (1024 * 1) + (1024 * 3) ) /* Additional 3kb of stack is added for enabling the prints */
#else
    #define CY_MQTT_DISCONNECT_EVENT_THREAD_STACK_SIZE       ( 1024 * 1 )
#endif

#define CY_MQTT_DISCONNECT_EVENT_THREAD_PRIORITY             ( CY_RTOS_PRIORITY_NORMAL )
#define CY_MQTT_DISCONNECT_EVENT_QUEUE_TIMEOUT_IN_MSEC       ( 500 )

/******************************************************
 *                    Constants
 ******************************************************/

/******************************************************
 *                   Enumerations
 ******************************************************/

/******************************************************
 *                 Type Definitions
 ******************************************************/

/******************************************************
 *                    Structures
 ******************************************************/

/**
 * Structure to keep the MQTT PUBLISH packets until an ACK is received
 * for QoS1 and QoS2 publishes.
 */
typedef struct publishpackets
{
    uint16_t               packetid;
    MQTTPublishInfo_t      pubinfo;
} cy_mqtt_pubpack_t;

/**
 * Structure to keep the MQTT PUBLISH packet ACK information
 * for QoS1 and QoS2 publishes.
 */
typedef struct publishpacket_ackstatus
{
    uint16_t      packetid;
    bool          puback_status;
} cy_mqtt_pub_ack_status_t;

/*
 * MQTT handle
 */
typedef struct mqtt_object
{
    bool                            mqtt_obj_initialized;      /**< MQTT object init status. */
    bool                            mqtt_secure_mode;          /**< MQTT secured mode. True if secure connection; false otherwise. */
    bool                            mqtt_session_established;  /**< MQTT client session establishment status. */
    bool                            broker_session_present;    /**< Broker session status. */
    bool                            mqtt_conn_status;          /**< MQTT network connect status. */
    uint8_t                         mqtt_obj_index;            /**< MQTT object index in mqtt_handle_database. */
    NetworkContext_t                network_context;           /**< MQTT Network context. */
    MQTTContext_t                   mqtt_context;              /**< MQTT context. */
    cy_awsport_server_info_t        server_info;               /**< MQTT broker info. */
    cy_awsport_ssl_credentials_t    security;                  /**< MQTT secure connection credentials. */
    cy_thread_t                     recv_thread;               /**< Receive thread handle. */
    cy_mqtt_callback_t              mqtt_event_cb;             /**< MQTT application callback for events. */
    MQTTSubAckStatus_t              sub_ack_status[ CY_MQTT_MAX_OUTGOING_SUBSCRIBES ]; /**< MQTT SUBSCRIBE command ACK status. */
    uint8_t                         num_of_subs_in_req;        /**< Number of subscription messages in outstanding MQTT subscribe request. */
    bool                            unsub_ack_received;        /**< Status of unsubscribe acknowledgment. */
    cy_mqtt_pub_ack_status_t        pub_ack_status;            /**< MQTT PUBLISH packetack received status. */
    uint16_t                        sent_packet_id;            /**< MQTT packet ID. */
    cy_mqtt_pubpack_t               outgoing_pub_packets[ CY_MQTT_MAX_OUTGOING_PUBLISHES ]; /**< MQTT PUBLISH packet. */
    cy_mutex_t                      process_mutex;             /**< Mutex for synchronizing MQTT object members. */
    void                            *user_data;                /**< User data which needs to be sent while calling registered app callback. */
} cy_mqtt_object_t ;

/*
 * MQTT handle database
 */
typedef struct mqtt_data_base
{
    cy_mqtt_t       *mqtt_handle;
    MQTTContext_t   *mqtt_context;
} mqtt_data_base_t ;

/******************************************************
 *               Static Function Declarations
 ******************************************************/

/******************************************************
 *                 Global Variables
 ******************************************************/
static mqtt_data_base_t  mqtt_handle_database[ CY_MQTT_MAX_HANDLE ];
static uint8_t           mqtt_handle_count = 0;
static cy_mutex_t        mqtt_db_mutex;
static bool              mqtt_lib_init_status = false;
static bool              mqtt_db_mutex_init_status = false;
static cy_thread_t       mqtt_disconnect_event_thread = NULL;
static cy_queue_t        mqtt_disconnect_event_queue = NULL;

/******************************************************
 *               Function Definitions
 ******************************************************/
static cy_rslt_t mqtt_cleanup_outgoing_publish( cy_mqtt_object_t *mqtt_obj, uint8_t index )
{
    if( index >= CY_MQTT_MAX_OUTGOING_PUBLISHES )
    {
        cy_mqtt_log_msg( CYLF_MIDDLEWARE, CY_LOG_ERR, "\n Bad arguments to mqtt_cleanup_outgoing_publish." );
        return CY_RSLT_MODULE_MQTT_BADARG;
    }
    /* Clear the outgoing PUBLISH packet. */
    ( void ) memset( &( mqtt_obj->outgoing_pub_packets[ index ] ), 0x00, sizeof( mqtt_obj->outgoing_pub_packets[ index ] ) );
    return CY_RSLT_SUCCESS;
}

/*----------------------------------------------------------------------------------------------------------*/

static cy_rslt_t mqtt_cleanup_outgoing_publish_with_packet_id( cy_mqtt_object_t *mqtt_obj, uint16_t packetid )
{
    cy_rslt_t  result = CY_RSLT_SUCCESS;
    uint8_t    index;

    if( (mqtt_obj == NULL) || (packetid == MQTT_PACKET_ID_INVALID) )
    {
        cy_mqtt_log_msg( CYLF_MIDDLEWARE, CY_LOG_ERR, "\n Bad arguments to mqtt_cleanup_outgoing_publish_with_packet_id." );
        return CY_RSLT_MODULE_MQTT_BADARG;
    }

    /* Clean up all saved outgoing PUBLISH packets. */
    for( index = 0; index < CY_MQTT_MAX_OUTGOING_PUBLISHES; index++ )
    {
        if( mqtt_obj->outgoing_pub_packets[ index ].packetid == packetid )
        {
            result = mqtt_cleanup_outgoing_publish( mqtt_obj, index );
            if( result != CY_RSLT_SUCCESS )
            {
                cy_mqtt_log_msg( CYLF_MIDDLEWARE, CY_LOG_DEBUG, "\nmqtt_cleanup_outgoing_publish failed with Error : [0x%X] ", (unsigned int)result );
                break;
            }
            cy_mqtt_log_msg( CYLF_MIDDLEWARE, CY_LOG_DEBUG, "\nCleaned up outgoing PUBLISH packet with packet id %u.\n\n", packetid );
            break;
        }
    }
    return result;
}

/*----------------------------------------------------------------------------------------------------------*/

static cy_rslt_t mqtt_update_suback_status( cy_mqtt_object_t *mqtt_obj, MQTTPacketInfo_t *packet_info )
{
    uint8_t        *payload = NULL, i = 0;
    size_t         num_of_subscriptions = 0;
    MQTTStatus_t   mqttStatus = MQTTSuccess;

    mqttStatus = MQTT_GetSubAckStatusCodes( packet_info, &payload, &num_of_subscriptions );
    if( (mqttStatus != MQTTSuccess) || (num_of_subscriptions != mqtt_obj->num_of_subs_in_req) )
    {
        cy_mqtt_log_msg( CYLF_MIDDLEWARE, CY_LOG_ERR, "\n MQTT_GetSubAckStatusCodes failed with status = %s.", MQTT_Status_strerror( mqttStatus ) );
        /* SubAckStatusCodes are not available for outstanding subscription messages waiting for acknowledgment. So Setting num_of_subs_in_req to 0.*/
        mqtt_obj->num_of_subs_in_req = 0;
        return CY_RSLT_MODULE_MQTT_ERROR;
    }
    ( void ) mqttStatus;

    for( i = 0; i < mqtt_obj->num_of_subs_in_req; i++ )
    {
        mqtt_obj->sub_ack_status[i] = (MQTTSubAckStatus_t)payload[ i ];
    }
    /* All outstanding subscription message acknowledgment status is updated. So Setting num_of_subs_in_req to 0. */
    mqtt_obj->num_of_subs_in_req = 0;
    return CY_RSLT_SUCCESS;
}

/*----------------------------------------------------------------------------------------------------------*/

static cy_rslt_t mqtt_get_next_free_index_for_publish( cy_mqtt_object_t *mqtt_obj, uint8_t *pindex )
{
    cy_rslt_t result = CY_RSLT_SUCCESS;
    uint8_t   index  = 0;
    bool slot_found = false;

    if( (mqtt_obj == NULL) || (pindex == NULL) )
    {
        cy_mqtt_log_msg( CYLF_MIDDLEWARE, CY_LOG_ERR, "\nBad arguments to mqtt_get_next_free_index_for_publish." );
        return CY_RSLT_MODULE_MQTT_BADARG;
    }

    for( index = 0; index < CY_MQTT_MAX_OUTGOING_PUBLISHES; index++ )
    {
        /* A free index is marked by the invalid packet ID.
         * Check if the the index has a free slot. */
        if( mqtt_obj->outgoing_pub_packets[ index ].packetid == MQTT_PACKET_ID_INVALID )
        {
            result = CY_RSLT_SUCCESS;
            slot_found = true;
            break;
        }
    }

    if( slot_found == true )
    {
        /* Copy the available index into the output param. */
        *pindex = index;
    }
    else
    {
        result = CY_RSLT_MODULE_MQTT_ERROR;
    }

    return result;
}

/*----------------------------------------------------------------------------------------------------------*/

static cy_rslt_t mqtt_cleanup_outgoing_publishes( cy_mqtt_object_t *mqtt_obj )
{
    if( mqtt_obj == NULL )
    {
        cy_mqtt_log_msg( CYLF_MIDDLEWARE, CY_LOG_ERR, "\nBad arguments to mqtt_cleanup_outgoing_publishes." );
        return CY_RSLT_MODULE_MQTT_BADARG;
    }

    /* Clean up all outgoing PUBLISH packets. */
    ( void ) memset( mqtt_obj->outgoing_pub_packets, 0x00, sizeof( mqtt_obj->outgoing_pub_packets ) );
    return CY_RSLT_SUCCESS;
}

/*----------------------------------------------------------------------------------------------------------*/

static cy_rslt_t mqtt_handle_publish_resend( cy_mqtt_object_t *mqtt_obj )
{
    cy_rslt_t         result = CY_RSLT_SUCCESS;
    MQTTStatus_t      mqttStatus = MQTTSuccess;
    uint8_t           index = 0U;
    MQTTStateCursor_t cursor = MQTT_STATE_CURSOR_INITIALIZER;
    uint16_t          packetid_to_resend = MQTT_PACKET_ID_INVALID;
    bool              found_packetid = false;

    /* MQTT_PublishToResend() provides a packet ID of the next PUBLISH packet
     * that should be resent. In accordance with the MQTT v3.1.1 spec,
     * MQTT_PublishToResend() preserves the ordering of when the original
     * PUBLISH packets were sent. The outgoing_pub_packets array is searched
     * through for the associated packet ID. */
    packetid_to_resend = MQTT_PublishToResend( &(mqtt_obj->mqtt_context), &cursor );
    while( packetid_to_resend != MQTT_PACKET_ID_INVALID )
    {
        found_packetid = false;

        for( index = 0U; index < CY_MQTT_MAX_OUTGOING_PUBLISHES; index++ )
        {
            if( mqtt_obj->outgoing_pub_packets[ index ].packetid == packetid_to_resend )
            {
                found_packetid = true;
                if( mqtt_obj->outgoing_pub_packets[ index ].pubinfo.qos != MQTTQoS0 )
                {
                    mqtt_obj->outgoing_pub_packets[ index ].pubinfo.dup = true;

                    cy_mqtt_log_msg( CYLF_MIDDLEWARE, CY_LOG_DEBUG, "\nSending duplicate PUBLISH with packet id %u.",
                                     mqtt_obj->outgoing_pub_packets[ index ].packetid );
                    mqttStatus = MQTT_Publish( &(mqtt_obj->mqtt_context), &(mqtt_obj->outgoing_pub_packets[ index ].pubinfo),
                                               mqtt_obj->outgoing_pub_packets[ index ].packetid );
                    if( mqttStatus != MQTTSuccess )
                    {
                        cy_mqtt_log_msg( CYLF_MIDDLEWARE, CY_LOG_ERR, "\nSending duplicate PUBLISH for packet id %u failed with status %s.",
                                         mqtt_obj->outgoing_pub_packets[ index ].packetid, MQTT_Status_strerror( mqttStatus ) );
                        result = CY_RSLT_MODULE_MQTT_PUBLISH_FAIL;
                        break;
                    }
                    else
                    {
                        cy_mqtt_log_msg( CYLF_MIDDLEWARE, CY_LOG_DEBUG, "\nSent duplicate PUBLISH successfully for packet id %u.\n\n",
                                         mqtt_obj->outgoing_pub_packets[ index ].packetid );
                    }
                }
                else
                {
                    cy_mqtt_log_msg( CYLF_MIDDLEWARE, CY_LOG_DEBUG, "\nResending PUBLISH packet id %u. is not required as its having QoS0\n\n",
                                     mqtt_obj->outgoing_pub_packets[ index ].packetid );
                }

            }
        }

        if( found_packetid == false )
        {
            cy_mqtt_log_msg( CYLF_MIDDLEWARE, CY_LOG_ERR, "\nPacket id %u requires resend, but was not found in outgoing_pub_packets.",
                             packetid_to_resend );
            result = CY_RSLT_MODULE_MQTT_PUBLISH_FAIL;
            break;
        }
        else
        {
            /* Get the next packetID to be resent. */
            packetid_to_resend = MQTT_PublishToResend( &(mqtt_obj->mqtt_context), &cursor );
        }
    }

    return result;
}

/*----------------------------------------------------------------------------------------------------------*/

static void mqtt_awsport_network_disconnect_callback( void *user_data )
{
    cy_rslt_t         result = CY_RSLT_SUCCESS;

    cy_mqtt_log_msg( CYLF_MIDDLEWARE, CY_LOG_INFO, "\n Network disconnection notification from socket layer.\n" );

    result = cy_rtos_put_queue( &mqtt_disconnect_event_queue, (void *)&user_data, CY_MQTT_DISCONNECT_EVENT_QUEUE_TIMEOUT_IN_MSEC, false );
    if( result != CY_RSLT_SUCCESS )
    {
        cy_mqtt_log_msg( CYLF_MIDDLEWARE, CY_LOG_ERR, "\nPushing to disconnect event queue failed with Error : [0x%X] ", (unsigned int)result );
    }

    return;
}

/*----------------------------------------------------------------------------------------------------------*/

static void mqtt_event_callback( MQTTContext_t *param_mqtt_context,
                                 MQTTPacketInfo_t *param_packet_info,
                                 MQTTDeserializedInfo_t *param_deserialized_info )
{
    cy_rslt_t         result = CY_RSLT_SUCCESS;
    uint16_t          packet_id;
    cy_mqtt_object_t  *mqtt_obj = NULL;
    cy_mqtt_t         handle = NULL;
    uint8_t           index = 0;
    cy_mqtt_event_t   event;

    if( (param_mqtt_context == NULL) || (param_packet_info == NULL) || (param_deserialized_info == NULL) )
    {
        cy_mqtt_log_msg( CYLF_MIDDLEWARE, CY_LOG_ERR, "\n Bad arguments to mqtt_event_callback." );
        return;
    }

    memset( &event, 0x00, sizeof(cy_mqtt_event_t) );

    for( index = 0 ; index < CY_MQTT_MAX_HANDLE ; index++ )
    {
        if( mqtt_handle_database[index].mqtt_context ==  param_mqtt_context )
        {
            handle = mqtt_handle_database[index].mqtt_handle;
            break;
        }
    }

    if( handle == NULL )
    {
        cy_mqtt_log_msg( CYLF_MIDDLEWARE, CY_LOG_ERR, "\n Invalid MQTT Context.." );
        return;
    }

    mqtt_obj = ( cy_mqtt_object_t * )handle;
    packet_id = param_deserialized_info->packetIdentifier;

    /* Handle incoming PUBLISH packets. The lower 4 bits of the PUBLISH packet
     * type is used for the dup, QoS, and retain flags. Therefore, masking
     * out the lower bits to check whether the packet is a PUBLISH packet. */
    if( ( param_packet_info->type & 0xF0U ) == MQTT_PACKET_TYPE_PUBLISH )
    {
        if( param_deserialized_info->pPublishInfo != NULL )
        {
            /* Handle incoming PUBLISH packets. */
            event.type = CY_MQTT_EVENT_TYPE_PUBLISH_RECEIVE;
            event.data.pub_msg.packet_id = packet_id;
            event.data.pub_msg.received_message.dup =  param_deserialized_info->pPublishInfo->dup;
            event.data.pub_msg.received_message.payload = (const char *) (param_deserialized_info->pPublishInfo->pPayload);
            event.data.pub_msg.received_message.payload_len = param_deserialized_info->pPublishInfo->payloadLength;

            if( param_deserialized_info->pPublishInfo->qos == MQTTQoS0 )
            {
                event.data.pub_msg.received_message.qos = CY_MQTT_QOS0;
            }
            if( param_deserialized_info->pPublishInfo->qos == MQTTQoS1 )
            {
                event.data.pub_msg.received_message.qos = CY_MQTT_QOS1;
            }
            if( param_deserialized_info->pPublishInfo->qos == MQTTQoS2 )
            {
                event.data.pub_msg.received_message.qos = CY_MQTT_QOS2;
            }
            event.data.pub_msg.received_message.retain = param_deserialized_info->pPublishInfo->retain;
            event.data.pub_msg.received_message.topic = param_deserialized_info->pPublishInfo->pTopicName;
            event.data.pub_msg.received_message.topic_len = param_deserialized_info->pPublishInfo->topicNameLength;
            if( mqtt_obj->mqtt_event_cb != NULL )
            {
                mqtt_obj->mqtt_event_cb( handle, event, mqtt_obj->user_data );
            }
        }
        else
        {
            cy_mqtt_log_msg( CYLF_MIDDLEWARE, CY_LOG_ERR, "\n Invalid pPublishInfo.." );
            return;
        }
    }
    else
    {
        /* Handle other packets. */
        switch( param_packet_info->type )
        {
            case MQTT_PACKET_TYPE_SUBACK:

                /* Make sure that the ACK packet identifier matches with the Request packet identifier. */
                if( mqtt_obj->sent_packet_id != packet_id )
                {
                    cy_mqtt_log_msg( CYLF_MIDDLEWARE, CY_LOG_ERR, "\nSUBACK packet identifier does not matches with Request packet identifier." );
                }
                else
                {
                    /* A SUBACK from the broker, containing the server response to our subscription request, has been received.
                     * It contains the status code indicating server approval/rejection for the subscription to the single topic
                     * requested. The SUBACK will be parsed to obtain the status code; this status code will be stored in the MQTT object
                     * member 'sub_ack_status'. */
                    result = mqtt_update_suback_status( mqtt_obj, param_packet_info );
                    if( result != CY_RSLT_SUCCESS )
                    {
                        cy_mqtt_log_msg( CYLF_MIDDLEWARE, CY_LOG_ERR, "\n mqtt_update_suback_status failed..!\n" );
                    }
                    cy_mqtt_log_msg( CYLF_MIDDLEWARE, CY_LOG_DEBUG, "\nSUBACK packet identifier matches with Request packet identifier." );
                }
                break;

            case MQTT_PACKET_TYPE_UNSUBACK:
                /* Make sure that the UNSUBACK packet identifier matches with the Request packet identifier. */
                if( mqtt_obj->sent_packet_id != packet_id )
                {
                    cy_mqtt_log_msg( CYLF_MIDDLEWARE, CY_LOG_ERR, "\nUNSUBACK packet identifier does not matches with Request packet identifier." );
                    mqtt_obj->unsub_ack_received = false;
                }
                else
                {
                    cy_mqtt_log_msg( CYLF_MIDDLEWARE, CY_LOG_DEBUG, "\nUNSUBACK packet identifier matches with Request packet identifier." );
                    mqtt_obj->unsub_ack_received = true;
                }
                break;

            case MQTT_PACKET_TYPE_PINGRESP:
                if( param_deserialized_info->deserializationResult != MQTTSuccess )
                {
                    memset( &event, 0x00, sizeof(cy_mqtt_event_t) );
                    event.type = CY_MQTT_EVENT_TYPE_DISCONNECT;
                    event.data.reason = CY_MQTT_DISCONN_TYPE_BROKER_DOWN;
                    if( mqtt_obj->mqtt_event_cb != NULL )
                    {
                        mqtt_obj->mqtt_event_cb( handle, event, mqtt_obj->user_data );
                    }
                    mqtt_obj->mqtt_session_established = false;
                }
                cy_mqtt_log_msg( CYLF_MIDDLEWARE, CY_LOG_DEBUG, "\nPing response received." );
                break;

            case MQTT_PACKET_TYPE_PUBREC:
                cy_mqtt_log_msg( CYLF_MIDDLEWARE, CY_LOG_DEBUG, "\nPUBREC received for packet id %u.\n\n", packet_id );
                if( param_deserialized_info->deserializationResult != MQTTSuccess )
                {
                    cy_mqtt_log_msg( CYLF_MIDDLEWARE, CY_LOG_INFO, "\nPUBREC received with status %s.", MQTT_Status_strerror( param_deserialized_info->deserializationResult ) );
                }
                else
                {
                    if( packet_id == mqtt_obj->pub_ack_status.packetid )
                    {
                        mqtt_obj->pub_ack_status.puback_status = true;
                    }
                    else
                    {
                        mqtt_obj->pub_ack_status.puback_status = false;
                    }
                }
                /* Clean up the PUBLISH packet when a PUBREC is received. */
                (void)mqtt_cleanup_outgoing_publish_with_packet_id( mqtt_obj, packet_id );
                break;

            case MQTT_PACKET_TYPE_PUBREL:
                cy_mqtt_log_msg( CYLF_MIDDLEWARE, CY_LOG_DEBUG, "\nPUBREL received for packet id %u.\n", packet_id );
                break;

            case MQTT_PACKET_TYPE_PUBCOMP:
                cy_mqtt_log_msg( CYLF_MIDDLEWARE, CY_LOG_DEBUG, "\nPUBCOMP received for packet id %u.\n\n", packet_id );
                break;

            case MQTT_PACKET_TYPE_PUBACK:
                cy_mqtt_log_msg( CYLF_MIDDLEWARE, CY_LOG_DEBUG, "\nPUBACK received for packet id %u.\n\n", packet_id );
                if( param_deserialized_info->deserializationResult != MQTTSuccess )
                {
                    cy_mqtt_log_msg( CYLF_MIDDLEWARE, CY_LOG_INFO, "\nPUBACK received with status %s.", MQTT_Status_strerror( param_deserialized_info->deserializationResult ) );
                }
                else
                {
                    if( packet_id == mqtt_obj->pub_ack_status.packetid )
                    {
                        mqtt_obj->pub_ack_status.puback_status = true;
                    }
                    else
                    {
                        mqtt_obj->pub_ack_status.puback_status = false;
                    }
                }
                /* Clean up the PUBLISH packet when a PUBACK is received. */
                (void)mqtt_cleanup_outgoing_publish_with_packet_id( mqtt_obj, packet_id );
                break;

            case MQTT_PACKET_TYPE_DISCONNECT:
                /* Because this is user-initiated disconnection, no need to notify the application.*/
                cy_mqtt_log_msg( CYLF_MIDDLEWARE, CY_LOG_DEBUG, "\nDisconnect packet received:(%02x).\n\n", param_packet_info->type );
                break;

            /* Any other packet type is invalid. */
            default:
                cy_mqtt_log_msg( CYLF_MIDDLEWARE, CY_LOG_DEBUG, "\nUnknown packet type received:(%02x).\n\n", param_packet_info->type );
        }
    }
}

/*----------------------------------------------------------------------------------------------------------*/

static cy_rslt_t mqtt_establish_session( cy_mqtt_object_t *mqtt_obj,
                                         MQTTConnectInfo_t *connect_info,
                                         MQTTPublishInfo_t *will_msg,
                                         bool create_clean_session,
                                         bool *session_present )
{
    cy_rslt_t         result = CY_RSLT_SUCCESS;
    MQTTStatus_t      mqttStatus = MQTTSuccess;

    /* Establish an MQTT session by sending a CONNECT packet. */

    result = cy_rtos_get_mutex( &(mqtt_obj->process_mutex), CY_RTOS_NEVER_TIMEOUT );
    if( result != CY_RSLT_SUCCESS )
    {
        cy_mqtt_log_msg( CYLF_MIDDLEWARE, CY_LOG_ERR, "\ncy_rtos_get_mutex for Mutex %p failed with Error : [0x%X] ", mqtt_obj->process_mutex, (unsigned int)result );
        return CY_RSLT_MODULE_MQTT_CONNECT_FAIL;
    }
    cy_mqtt_log_msg( CYLF_MIDDLEWARE, CY_LOG_DEBUG, "\nmqtt_establish_session - Acquired Mutex %p ", mqtt_obj->process_mutex );

    /* Send an MQTT CONNECT packet to the broker. */
    mqttStatus = MQTT_Connect( &(mqtt_obj->mqtt_context), connect_info, will_msg, CY_MQTT_CONNACK_RECV_TIMEOUT_MS, session_present );
    if( mqttStatus != MQTTSuccess )
    {
        cy_mqtt_log_msg( CYLF_MIDDLEWARE, CY_LOG_ERR, "\nConnection with MQTT broker failed with status %s.", MQTT_Status_strerror( mqttStatus ) );

        result = cy_rtos_set_mutex( &(mqtt_obj->process_mutex) );
        if( result != CY_RSLT_SUCCESS )
        {
            cy_mqtt_log_msg( CYLF_MIDDLEWARE, CY_LOG_ERR, "\ncy_rtos_set_mutex for Mutex %p failed with Error : [0x%X] ", (unsigned int)result );
        }
        cy_mqtt_log_msg( CYLF_MIDDLEWARE, CY_LOG_DEBUG, "\nmqtt_establish_session - Released Mutex %p ", mqtt_obj->process_mutex );
        return CY_RSLT_MODULE_MQTT_CONNECT_FAIL;
    }
    else
    {
        cy_mqtt_log_msg( CYLF_MIDDLEWARE, CY_LOG_DEBUG, "\nMQTT connection successfully established with broker.\n\n" );
        mqtt_obj->mqtt_session_established = true;
    }

    result = cy_rtos_set_mutex( &(mqtt_obj->process_mutex) );
    if( result != CY_RSLT_SUCCESS )
    {
        cy_mqtt_log_msg( CYLF_MIDDLEWARE, CY_LOG_ERR, "\ncy_rtos_set_mutex for Mutex %p failed with Error : [0x%X] ", (unsigned int)result );
        return CY_RSLT_MODULE_MQTT_CONNECT_FAIL;
    }
    cy_mqtt_log_msg( CYLF_MIDDLEWARE, CY_LOG_DEBUG, "\nmqtt_establish_session - Released Mutex %p ", mqtt_obj->process_mutex );

    return result;
}

/*----------------------------------------------------------------------------------------------------------*/
int32_t mqtt_awsport_network_receive( NetworkContext_t *network_context, void *buffer, size_t bytes_recv )
{
    int32_t bytes_received = 0, total_received = 0;
    size_t entryTimeMs = 0U, exitTimeMs = 0, remainingTimeMs = 0, elapsedTimeMs = 0U;;
    size_t bytestoread = 0;

    remainingTimeMs = CY_MQTT_MESSAGE_RECEIVE_TIMEOUT_MS;

    do
    {
        bytestoread = (size_t)bytes_recv - total_received;
        entryTimeMs = Clock_GetTimeMs();
        bytes_received = cy_awsport_network_receive( network_context, (void *)((char *)buffer + total_received), bytestoread );
        exitTimeMs = Clock_GetTimeMs();
        elapsedTimeMs = exitTimeMs - entryTimeMs;
        if( bytes_received < 0 )
        {
            return bytes_received;
        }
        else if( bytes_received == 0 )
        {
            if( total_received == 0 )
            {
                /* No data in the socket, so return. */
                break;
            }
        }
        else
        {
            total_received = total_received + bytes_received;
            cy_mqtt_log_msg( CYLF_MIDDLEWARE, CY_LOG_DEBUG, "\n Total Bytes Received = %u", (unsigned int)total_received );
            /* Reset the wait time as some data is received. */
            elapsedTimeMs = 0;
            remainingTimeMs = CY_MQTT_MESSAGE_RECEIVE_TIMEOUT_MS;
        }
        remainingTimeMs = remainingTimeMs - elapsedTimeMs;
    } while( (total_received < bytes_recv) && (remainingTimeMs > 0) );

    return total_received;
}

/*----------------------------------------------------------------------------------------------------------*/

static cy_rslt_t mqtt_initialize_core_lib( MQTTContext_t *param_mqtt_context,
                                           NetworkContext_t *param_network_context,
                                           uint8_t *networkbuff, uint32_t buff_len )
{
    cy_rslt_t            result = CY_RSLT_SUCCESS;
    MQTTStatus_t         mqttStatus = MQTTSuccess;
    MQTTFixedBuffer_t    networkBuffer;
    TransportInterface_t transport;

    if( (param_mqtt_context == NULL) || (param_network_context == NULL) )
    {
        cy_mqtt_log_msg( CYLF_MIDDLEWARE, CY_LOG_ERR, "\n Bad arguments to mqtt_initialize_core_lib." );
        return CY_RSLT_MODULE_MQTT_BADARG;
    }

    memset( &networkBuffer, 0x00, sizeof(MQTTFixedBuffer_t) );
    memset( &transport, 0x00, sizeof(TransportInterface_t) );

    /* Fill in TransportInterface send and receive function pointers. */
    transport.pNetworkContext = param_network_context;
    transport.send = (TransportSend_t)&cy_awsport_network_send;
    transport.recv = (TransportRecv_t)&mqtt_awsport_network_receive;

    /* Fill the values for the network buffer. */
    networkBuffer.pBuffer = networkbuff;
    networkBuffer.size = buff_len;

    /* Initialize the MQTT library. */
    mqttStatus = MQTT_Init( param_mqtt_context, &transport, Clock_GetTimeMs,
                            mqtt_event_callback, &networkBuffer );
    if( mqttStatus != MQTTSuccess )
    {
        cy_mqtt_log_msg( CYLF_MIDDLEWARE, CY_LOG_ERR, "\n MQTT init failed with Status = %s.", MQTT_Status_strerror( mqttStatus ) );
        result = CY_RSLT_MODULE_MQTT_INIT_FAIL;
    }

    return result;
}

/*----------------------------------------------------------------------------------------------------------*/

static void mqtt_disconn_event_thread( cy_thread_arg_t arg )
{
    cy_rslt_t         result = CY_RSLT_SUCCESS;
    cy_mqtt_t         handle = NULL;
    cy_mqtt_object_t  *mqtt_obj = NULL;
    cy_mqtt_event_t   event;
    (void)arg;

    cy_mqtt_log_msg( CYLF_MIDDLEWARE, CY_LOG_INFO, "\nStarting mqtt_disconn_event_thread...\n" );

    while( true )
    {
        result = cy_rtos_get_queue( &mqtt_disconnect_event_queue, (void *)&handle, CY_RTOS_NEVER_TIMEOUT, false );
        if( CY_RSLT_SUCCESS != result )
        {
            continue;
        }

        memset( &event, 0x00, sizeof(cy_mqtt_event_t) );
        event.type = CY_MQTT_EVENT_TYPE_DISCONNECT;

        if( handle == NULL )
        {
            cy_mqtt_log_msg( CYLF_MIDDLEWARE, CY_LOG_ERR, "\nInvalid mqtt handle...!\n" );
        }
        else
        {
            mqtt_obj = (cy_mqtt_object_t *)handle;
            cy_mqtt_log_msg( CYLF_MIDDLEWARE, CY_LOG_DEBUG, "\nmqtt_awsport_network_disconnect_callback - Acquiring Mutex %p ", mqtt_obj->process_mutex );
            result = cy_rtos_get_mutex( &(mqtt_obj->process_mutex), CY_RTOS_NEVER_TIMEOUT );
            if( result != CY_RSLT_SUCCESS )
            {
                cy_mqtt_log_msg( CYLF_MIDDLEWARE, CY_LOG_ERR, "\ncy_rtos_get_mutex for Mutex %p failed with Error : [0x%X] ", mqtt_obj->process_mutex, (unsigned int)result );
                continue;
            }
            cy_mqtt_log_msg( CYLF_MIDDLEWARE, CY_LOG_DEBUG, "\nmqtt_awsport_network_disconnect_callback - Acquired Mutex %p ", mqtt_obj->process_mutex );

            if( mqtt_obj->mqtt_session_established == true )
            {
                event.data.reason = CY_MQTT_DISCONN_TYPE_NETWORK_DOWN;
                if( mqtt_obj->mqtt_event_cb != NULL )
                {
                    mqtt_obj->mqtt_event_cb( (cy_mqtt_t)mqtt_obj, event, mqtt_obj->user_data );
                }
                mqtt_obj->mqtt_session_established = false;
            }

            cy_mqtt_log_msg( CYLF_MIDDLEWARE, CY_LOG_DEBUG, "\nmqtt_awsport_network_disconnect_callback - Releasing Mutex %p ", mqtt_obj->process_mutex );
            result = cy_rtos_set_mutex( &(mqtt_obj->process_mutex) );
            if( result != CY_RSLT_SUCCESS )
            {
                cy_mqtt_log_msg( CYLF_MIDDLEWARE, CY_LOG_ERR, "\ncy_rtos_set_mutex for Mutex %p failed with Error : [0x%X] ", (unsigned int)result );
            }
            cy_mqtt_log_msg( CYLF_MIDDLEWARE, CY_LOG_DEBUG, "\nmqtt_awsport_network_disconnect_callback - Released Mutex %p ", mqtt_obj->process_mutex );
        }
    }
}

/*----------------------------------------------------------------------------------------------------------*/

static void mqtt_receive_thread( cy_thread_arg_t arg )
{
    cy_rslt_t         result = CY_RSLT_SUCCESS;
    cy_mqtt_object_t  *mqtt_obj = NULL;
    MQTTStatus_t      mqtt_status = MQTTSuccess;
    cy_mqtt_event_t   event;
    bool              connect_status = true;

    mqtt_obj = (cy_mqtt_object_t *)arg;

    if( (mqtt_obj == NULL) || (mqtt_obj->mqtt_obj_initialized == false) )
    {
        cy_mqtt_log_msg( CYLF_MIDDLEWARE, CY_LOG_ERR, "\nInvalid MQTT object..!\n" );
        return;
    }

    cy_mqtt_log_msg( CYLF_MIDDLEWARE, CY_LOG_DEBUG, "\nStarting MQTT Receive thread for MQTT handle : %p \n", (cy_mqtt_t)mqtt_obj );

    while( true )
    {
        result = cy_rtos_get_mutex( &(mqtt_obj->process_mutex), CY_RTOS_NEVER_TIMEOUT );
        if( result != CY_RSLT_SUCCESS )
        {
            cy_mqtt_log_msg( CYLF_MIDDLEWARE, CY_LOG_ERR, "\ncy_rtos_get_mutex for Mutex %p failed with Error : [0x%X] ", mqtt_obj->process_mutex, (unsigned int)result );
            break;
        }
        cy_mqtt_log_msg( CYLF_MIDDLEWARE, CY_LOG_DEBUG, "\nmqtt_receive_thread - Acquired Mutex %p ", mqtt_obj->process_mutex );

        connect_status = mqtt_obj->mqtt_session_established;
        if( connect_status )
        {
            mqtt_status = MQTT_ProcessLoop( &(mqtt_obj->mqtt_context), CY_MQTT_RECEIVE_DATA_TIMEOUT_MS );
            if( mqtt_status != MQTTSuccess )
            {
                if( (mqtt_status == MQTTRecvFailed)  || (mqtt_status == MQTTSendFailed) ||
                    (mqtt_status == MQTTBadResponse) || (mqtt_status == MQTTKeepAliveTimeout) ||
                    (mqtt_status == MQTTIllegalState ) )
                {
                    cy_mqtt_log_msg( CYLF_MIDDLEWARE, CY_LOG_ERR, "\nmqtt_receive_thread MQTT_ProcessLoop failed with status %s \n", MQTT_Status_strerror(mqtt_status) );
                    memset( &event, 0x00, sizeof(cy_mqtt_event_t) );

                    if( mqtt_status == MQTTKeepAliveTimeout )
                    {
                        event.type = CY_MQTT_EVENT_TYPE_DISCONNECT;
                        event.data.reason = CY_MQTT_DISCONN_TYPE_BROKER_DOWN;
                        if( mqtt_obj->mqtt_event_cb != NULL )
                        {
                            mqtt_obj->mqtt_event_cb( (cy_mqtt_t)mqtt_obj, event, mqtt_obj->user_data );
                        }
                        mqtt_obj->mqtt_session_established = false;
                    }
                }
            }
        }

        result = cy_rtos_set_mutex( &(mqtt_obj->process_mutex) );
        if( result != CY_RSLT_SUCCESS )
        {
            cy_mqtt_log_msg( CYLF_MIDDLEWARE, CY_LOG_ERR, "\ncy_rtos_set_mutex for Mutex %p failed with Error : [0x%X] ", (unsigned int)result );
        }
        cy_mqtt_log_msg( CYLF_MIDDLEWARE, CY_LOG_DEBUG, "\nmqtt_receive_thread - Released Mutex %p ", mqtt_obj->process_mutex );

        cy_rtos_delay_milliseconds( CY_MQTT_RECEIVE_THREAD_SLEEP_MS );
    }

    return;
}

/*----------------------------------------------------------------------------------------------------------*/

cy_rslt_t cy_mqtt_init( void )
{
    cy_rslt_t result = CY_RSLT_SUCCESS;

    if( (mqtt_lib_init_status == true) )
    {
        cy_mqtt_log_msg( CYLF_MIDDLEWARE, CY_LOG_INFO, "\nMQTT library is already initialized. Number of MQTT client instance : [%d] \n", mqtt_handle_count );
        return result;
    }

    result = cy_rtos_init_mutex2( &mqtt_db_mutex, false );
    if( result != CY_RSLT_SUCCESS )
    {
        cy_mqtt_log_msg( CYLF_MIDDLEWARE, CY_LOG_ERR, "\nCreating new mutex %p. failed", mqtt_db_mutex );
        return result;
    }
    mqtt_db_mutex_init_status = true;

    result = cy_awsport_network_init();
    if( result != CY_RSLT_SUCCESS )
    {
        cy_mqtt_log_msg( CYLF_MIDDLEWARE, CY_LOG_ERR, "\ncy_awsport_network_init failed with Error : [0x%X] ", (unsigned int)result );
        (void)cy_rtos_deinit_mutex( &mqtt_db_mutex );
        mqtt_db_mutex_init_status = false;
        return result;
    }

    /*
     * Initialize the queue for disconnect events.
     */
    result = cy_rtos_init_queue( &mqtt_disconnect_event_queue, CY_MQTT_DISCONNECT_EVENT_QUEUE_SIZE, sizeof(cy_mqtt_object_t *) );
    if( result != CY_RSLT_SUCCESS )
    {
        cy_mqtt_log_msg( CYLF_MIDDLEWARE, CY_LOG_ERR, "\ncy_rtos_init_queue failed with Error : [0x%X] ", (unsigned int)result );
        (void)cy_rtos_deinit_mutex( &mqtt_db_mutex );
        (void)cy_awsport_network_deinit();
        mqtt_db_mutex_init_status = false;
        return result;
    }

    result = cy_rtos_create_thread( &mqtt_disconnect_event_thread, mqtt_disconn_event_thread, "MQTTdisconnectEventThread", NULL,
                                    CY_MQTT_DISCONNECT_EVENT_THREAD_STACK_SIZE, CY_MQTT_DISCONNECT_EVENT_THREAD_PRIORITY, NULL );
    if( result != CY_RSLT_SUCCESS )
    {
        cy_mqtt_log_msg( CYLF_MIDDLEWARE, CY_LOG_ERR, "\ncy_rtos_create_thread failed with Error : [0x%X] ", (unsigned int)result );
        (void)cy_rtos_deinit_mutex( &mqtt_db_mutex );
        (void)cy_awsport_network_deinit();
        (void)cy_rtos_deinit_queue( &mqtt_disconnect_event_queue );
        mqtt_db_mutex_init_status = false;
        return result;
    }

    mqtt_lib_init_status = true;
    cy_mqtt_log_msg( CYLF_MIDDLEWARE, CY_LOG_DEBUG, "\ncy_awsport_network_init successful." );

    return result;
}

/*----------------------------------------------------------------------------------------------------------*/

cy_rslt_t cy_mqtt_create( uint8_t *buffer, uint32_t bufflen,
                          cy_awsport_ssl_credentials_t *security,
                          cy_mqtt_broker_info_t *broker_info,
                          cy_mqtt_callback_t event_callback,
                          void *user_data,
                          cy_mqtt_t *mqtt_handle )
{
    cy_rslt_t         result = CY_RSLT_SUCCESS;
    cy_mqtt_object_t  *mqtt_obj = NULL;
    uint8_t           slot_index;
    bool              slot_found;
    bool              process_mutex_init_status = false;

    if( (broker_info == NULL) || (mqtt_handle == NULL) || (event_callback == NULL) )
    {
        cy_mqtt_log_msg( CYLF_MIDDLEWARE, CY_LOG_ERR, "\nBad arguments to cy_mqtt_create()..!\n" );
        return CY_RSLT_MODULE_MQTT_BADARG;
    }

    if( buffer == NULL )
    {
        cy_mqtt_log_msg( CYLF_MIDDLEWARE, CY_LOG_ERR, "\nInvalid network buffer..!\n" );
        return CY_RSLT_MODULE_MQTT_BADARG;
    }

    if( bufflen < CY_MQTT_MIN_NETWORK_BUFFER_SIZE )
    {
        cy_mqtt_log_msg( CYLF_MIDDLEWARE, CY_LOG_ERR, "\nBuffer length is less then minimun network buffer size : %u..!\n", (uint16_t)CY_MQTT_MIN_NETWORK_BUFFER_SIZE );
        return CY_RSLT_MODULE_MQTT_BADARG;
    }

    if( (mqtt_lib_init_status == false) || (mqtt_db_mutex_init_status == false) )
    {
        cy_mqtt_log_msg( CYLF_MIDDLEWARE, CY_LOG_ERR, "\nLibrary init is not done/Global mutex is not initialized..!\n " );
        return CY_RSLT_MODULE_MQTT_CREATE_FAIL;
    }

    result = cy_rtos_get_mutex( &mqtt_db_mutex, CY_RTOS_NEVER_TIMEOUT );
    if( result != CY_RSLT_SUCCESS )
    {
        cy_mqtt_log_msg( CYLF_MIDDLEWARE, CY_LOG_ERR, "\ncy_rtos_get_mutex for Mutex %p failed with Error : [0x%X] ", mqtt_db_mutex, (unsigned int)result );
        return result;
    }
    cy_mqtt_log_msg( CYLF_MIDDLEWARE, CY_LOG_DEBUG, "\ncy_mqtt_create - Acquired Mutex %p ", mqtt_db_mutex );

    if( mqtt_handle_count >= CY_MQTT_MAX_HANDLE )
    {
        cy_mqtt_log_msg( CYLF_MIDDLEWARE, CY_LOG_ERR, "\nNumber of created mqtt object exceeds %d..!\n", CY_MQTT_MAX_HANDLE );
        (void)cy_rtos_set_mutex( &mqtt_db_mutex );
        return CY_RSLT_MODULE_MQTT_CREATE_FAIL;
    }

    result = cy_rtos_set_mutex( &mqtt_db_mutex );
    if( result != CY_RSLT_SUCCESS )
    {
        cy_mqtt_log_msg( CYLF_MIDDLEWARE, CY_LOG_ERR, "\ncy_rtos_set_mutex for Mutex %p failed with Error : [0x%X] ", mqtt_db_mutex, (unsigned int)result );
        return result;
    }
    cy_mqtt_log_msg( CYLF_MIDDLEWARE, CY_LOG_DEBUG, "\ncy_mqtt_create - Released Mutex %p ", mqtt_db_mutex );

    mqtt_obj = (cy_mqtt_object_t *)malloc( sizeof( cy_mqtt_object_t ) );
    if( mqtt_obj == NULL )
    {
        cy_mqtt_log_msg( CYLF_MIDDLEWARE, CY_LOG_ERR, "\nMemory not available to create MQTT object..!\n" );
        return CY_RSLT_MODULE_MQTT_NOMEM;
    }
    cy_mqtt_log_msg( CYLF_MIDDLEWARE, CY_LOG_DEBUG, "\nmqtt_obj : %p..!\n", mqtt_obj );

    /* Clear the MQTT handle data. */
    memset( mqtt_obj, 0x00, sizeof( cy_mqtt_object_t ) );

    mqtt_obj->mqtt_obj_initialized = false;
    if( security != NULL )
    {
        mqtt_obj->security.alpnprotos = security->alpnprotos;
        mqtt_obj->security.alpnprotoslen = security->alpnprotoslen;
        mqtt_obj->security.sni_host_name = security->sni_host_name;
        mqtt_obj->security.sni_host_name_size = security->sni_host_name_size;
        mqtt_obj->security.username = security->username;
        mqtt_obj->security.username_size = security->username_size;
        mqtt_obj->security.password = security->password;
        mqtt_obj->security.password_size = security->password_size;

        mqtt_obj->security.client_cert = security->client_cert;
        mqtt_obj->security.client_cert_size = security->client_cert_size;
        mqtt_obj->security.private_key = security->private_key;
        mqtt_obj->security.private_key_size = security->private_key_size;
        mqtt_obj->security.root_ca = security->root_ca;
        mqtt_obj->security.root_ca_size = security->root_ca_size;
        mqtt_obj->mqtt_secure_mode = true;
    }
    else
    {
        mqtt_obj->mqtt_secure_mode = false;
    }

    mqtt_obj->server_info.host_name = broker_info->hostname;
    mqtt_obj->server_info.port = broker_info->port;
    mqtt_obj->mqtt_event_cb = event_callback;
    mqtt_obj->user_data = user_data;

    if( user_data == NULL )
    {
        cy_mqtt_log_msg( CYLF_MIDDLEWARE, CY_LOG_INFO, "\nArgument user_data is NULL..!\n" );
    }

    result = cy_rtos_init_mutex2( &(mqtt_obj->process_mutex), false );
    if( result != CY_RSLT_SUCCESS )
    {
        cy_mqtt_log_msg( CYLF_MIDDLEWARE, CY_LOG_ERR, "\nCreating new mutex %p. failed", mqtt_obj->process_mutex );
        goto exit;
    }

    process_mutex_init_status = true;

    result = mqtt_initialize_core_lib( &(mqtt_obj->mqtt_context), &(mqtt_obj->network_context), buffer, bufflen );
    if( result != CY_RSLT_SUCCESS )
    {
        cy_mqtt_log_msg( CYLF_MIDDLEWARE, CY_LOG_ERR, "\nmqtt_initialize_core_lib failed with Error : [0x%X] ", (unsigned int)result );
        goto exit;
    }
    else
    {
        cy_mqtt_log_msg( CYLF_MIDDLEWARE, CY_LOG_DEBUG, "\nmqtt_initialize_core_lib successful." );
    }

    mqtt_obj->network_context.disconnect_info.cbf = mqtt_awsport_network_disconnect_callback;
    mqtt_obj->network_context.disconnect_info.user_data = ( void * )mqtt_obj;

    result = cy_rtos_get_mutex( &mqtt_db_mutex, CY_RTOS_NEVER_TIMEOUT );
    if( result != CY_RSLT_SUCCESS )
    {
        cy_mqtt_log_msg( CYLF_MIDDLEWARE, CY_LOG_ERR, "\ncy_rtos_get_mutex for Mutex %p failed with Error : [0x%X] ", mqtt_db_mutex, (unsigned int)result );
        goto exit;
    }
    cy_mqtt_log_msg( CYLF_MIDDLEWARE, CY_LOG_DEBUG, "\ncy_mqtt_create - Acquired Mutex %p ", mqtt_db_mutex );

    slot_index = 0;
    slot_found = false;

    while( slot_index < CY_MQTT_MAX_HANDLE )
    {
        if( mqtt_handle_database[slot_index].mqtt_handle == NULL )
        {
            mqtt_handle_database[slot_index].mqtt_handle = (void *)mqtt_obj;
            mqtt_handle_database[slot_index].mqtt_context = &(mqtt_obj->mqtt_context);
            slot_found = true;
            break;
        }
        slot_index++;
    }

    if( slot_found == false )
    {
        cy_mqtt_log_msg( CYLF_MIDDLEWARE, CY_LOG_ERR, "\n Free slot not available for new handle..!\n" );
        (void)cy_rtos_set_mutex( &mqtt_db_mutex );
        result = CY_RSLT_MODULE_MQTT_CREATE_FAIL;
        goto exit;
    }

    mqtt_obj->mqtt_obj_initialized = true;
    mqtt_obj->mqtt_obj_index = slot_index;
    *mqtt_handle = (void *)mqtt_obj;
    mqtt_handle_count++;

    result = cy_rtos_set_mutex( &mqtt_db_mutex );
    if( result != CY_RSLT_SUCCESS )
    {
        cy_mqtt_log_msg( CYLF_MIDDLEWARE, CY_LOG_ERR, "\ncy_rtos_set_mutex for Mutex %p failed with Error : [0x%X] ", mqtt_db_mutex, (unsigned int)result );
        goto exit;
    }
    cy_mqtt_log_msg( CYLF_MIDDLEWARE, CY_LOG_DEBUG, "\ncy_mqtt_create - Released Mutex %p ", mqtt_db_mutex );

    cy_mqtt_log_msg( CYLF_MIDDLEWARE, CY_LOG_INFO, "\nMQTT object created successfully..\n" );
    return CY_RSLT_SUCCESS;

exit :
    if( mqtt_obj != NULL )
    {
        if( process_mutex_init_status == true )
        {
            (void)cy_rtos_deinit_mutex( &(mqtt_obj->process_mutex) );
            process_mutex_init_status = false;
        }
        cy_mqtt_log_msg( CYLF_MIDDLEWARE, CY_LOG_DEBUG, "\n Free mqtt_obj : %p..!\n", mqtt_obj );
        free( mqtt_obj );
    }

    return result;
}

/*----------------------------------------------------------------------------------------------------------*/

cy_rslt_t cy_mqtt_connect( cy_mqtt_t mqtt_handle, cy_mqtt_connect_info_t *connect_info )
{
    cy_rslt_t                     result = CY_RSLT_SUCCESS;
    cy_rslt_t                     res = CY_RSLT_SUCCESS;
    RetryUtilsParams_t            reconnectParams;
    MQTTStatus_t                  mqttStatus = MQTTSuccess;
    RetryUtilsStatus_t            retryUtilsStatus = RetryUtilsSuccess;
    cy_mqtt_object_t              *mqtt_obj;
    bool                          create_clean_session = false;
    MQTTConnectInfo_t             connect_details;
    MQTTPublishInfo_t             will_msg_details;
    MQTTPublishInfo_t             *will_msg_ptr = NULL;
    cy_awsport_ssl_credentials_t  *security = NULL;

    if( mqtt_handle == NULL )
    {
        cy_mqtt_log_msg( CYLF_MIDDLEWARE, CY_LOG_ERR, "\nBad arguments to cy_mqtt_connect()..!\n" );
        return CY_RSLT_MODULE_MQTT_BADARG;
    }

    mqtt_obj = (cy_mqtt_object_t *)mqtt_handle;
    if( mqtt_obj->mqtt_obj_initialized == false )
    {
        cy_mqtt_log_msg( CYLF_MIDDLEWARE, CY_LOG_ERR, "\nInvalid MQTT object..!\n" );
        return CY_RSLT_MODULE_MQTT_OBJ_NOT_INITIALIZED;
    }

    memset( &connect_details, 0x00, sizeof( MQTTConnectInfo_t ) );
    memset( &will_msg_details, 0x00, sizeof( MQTTPublishInfo_t ) );

    /* Connect Information */
    connect_details.cleanSession = connect_info->clean_session;
    connect_details.keepAliveSeconds = connect_info->keep_alive_sec;
    connect_details.pClientIdentifier = connect_info->client_id;
    connect_details.clientIdentifierLength = connect_info->client_id_len;
    connect_details.pPassword = connect_info->password;
    connect_details.passwordLength = connect_info->password_len;
    connect_details.pUserName = connect_info->username;
    connect_details.userNameLength = connect_info->username_len;

    if( connect_info->will_info != NULL )
    {
        /* Will information. */
        cy_mqtt_log_msg( CYLF_MIDDLEWARE, CY_LOG_DEBUG, "\nWill info is not NULL ..!\n" );

        if( connect_info->will_info->qos > CY_MQTT_QOS2 )
        {
            cy_mqtt_log_msg( CYLF_MIDDLEWARE, CY_LOG_ERR, "\nInvalid Will msg QoS..!\n" );
            return CY_RSLT_MODULE_MQTT_CONNECT_FAIL;
        }
        if( (connect_info->will_info->dup != true) && (connect_info->will_info->dup != false) )
        {
            cy_mqtt_log_msg( CYLF_MIDDLEWARE, CY_LOG_ERR, "\nInvalid Will msg dup..!\n" );
            return CY_RSLT_MODULE_MQTT_CONNECT_FAIL;
        }
        if( (connect_info->will_info->retain != true) && (connect_info->will_info->retain != false) )
        {
            cy_mqtt_log_msg( CYLF_MIDDLEWARE, CY_LOG_ERR, "\nInvalid Will msg retain..!\n" );
            return CY_RSLT_MODULE_MQTT_CONNECT_FAIL;
        }

        if( connect_info->will_info->qos == CY_MQTT_QOS0 )
        {
            will_msg_details.qos = MQTTQoS0;
        }
        else if( connect_info->will_info->qos == CY_MQTT_QOS1 )
        {
            will_msg_details.qos = MQTTQoS1;
        }
        else
        {
            will_msg_details.qos = MQTTQoS2;
        }

        will_msg_details.dup = connect_info->will_info->dup;
        will_msg_details.retain = connect_info->will_info->retain;
        will_msg_details.pTopicName = connect_info->will_info->topic;
        will_msg_details.topicNameLength = connect_info->will_info->topic_len;
        will_msg_details.pPayload = connect_info->will_info->payload;
        will_msg_details.payloadLength = connect_info->will_info->payload_len;
        will_msg_ptr = &will_msg_details;
    }
    else
    {
        cy_mqtt_log_msg( CYLF_MIDDLEWARE, CY_LOG_DEBUG, "\nWill info is NULL ..!\n" );
        will_msg_ptr = NULL;
    }

    /* Initialize the reconnect attempts and interval. */
    RetryUtils_ParamsReset( &reconnectParams );

    if( mqtt_obj->mqtt_secure_mode == true )
    {
        security = &(mqtt_obj->security);
    }
    else
    {
        security = NULL;
    }

    /* Attempt to connect to an MQTT broker. If connection fails, retry after
     * a timeout. The timeout value will exponentially increase until the maximum
     * attempts are reached.
     */
    do
    {
        cy_mqtt_log_msg( CYLF_MIDDLEWARE, CY_LOG_DEBUG, "\nCreating MQTT socket..\n" );
        result = cy_awsport_network_create( &(mqtt_obj->network_context), &(mqtt_obj->server_info), security, &(mqtt_obj->network_context.disconnect_info) );
        if ( result != CY_RSLT_SUCCESS )
        {
            cy_mqtt_log_msg( CYLF_MIDDLEWARE, CY_LOG_ERR, "\ncy_awsport_network_create failed with Error : [0x%X] ", (unsigned int)result );
            cy_mqtt_log_msg( CYLF_MIDDLEWARE, CY_LOG_ERR, "\nConnection to the broker failed. Retrying connection with backoff and jitter.\n" );
            retryUtilsStatus = RetryUtils_BackoffAndSleep( &reconnectParams );
        }
        else
        {
            /* Establish a TLS session with the MQTT broker. */
            cy_mqtt_log_msg( CYLF_MIDDLEWARE, CY_LOG_INFO, "Establishing a TLS session to %.*s:%d.",
                             strlen(mqtt_obj->server_info.host_name), mqtt_obj->server_info.host_name, mqtt_obj->server_info.port );
            result = cy_awsport_network_connect( &(mqtt_obj->network_context),
                                                 CY_MQTT_MESSAGE_SEND_TIMEOUT_MS,
                                                 CY_MQTT_SOCKET_RECEIVE_TIMEOUT_MS );
            if( result != CY_RSLT_SUCCESS )
            {
                cy_mqtt_log_msg( CYLF_MIDDLEWARE, CY_LOG_ERR, "\nConnection to the broker failed. Retrying connection with backoff and jitter.\n" );

                retryUtilsStatus = RetryUtils_BackoffAndSleep( &reconnectParams );
                (void)cy_awsport_network_delete( &(mqtt_obj->network_context) );
                /*
                 * In case of an unexpected network disconnection, the cy_awsport_network_delete API always returns failure. Therefore,
                 * the return value of the cy_awsport_network_delete API is not checked here.
                 */
                /* Fall-through. */
            }

            if( retryUtilsStatus == RetryUtilsRetriesExhausted )
            {
                cy_mqtt_log_msg( CYLF_MIDDLEWARE, CY_LOG_ERR, "\nConnection to the broker failed, all attempts exhausted.\n" );
                result = CY_RSLT_MODULE_MQTT_CONNECT_FAIL;
            }
        }

    } while( ( result != CY_RSLT_SUCCESS ) && ( retryUtilsStatus == RetryUtilsSuccess ) );

    if( result != CY_RSLT_SUCCESS )
    {
        cy_mqtt_log_msg( CYLF_MIDDLEWARE, CY_LOG_ERR, "\nTLS connection failed with Error : [0x%X] ", (unsigned int)result );
        return result;
    }

    cy_mqtt_log_msg( CYLF_MIDDLEWARE, CY_LOG_INFO, "\nTLS connection established ..\n" );

    create_clean_session = (connect_details.cleanSession == true ) ? false : true;
    if( create_clean_session == true )
    {
        cy_mqtt_log_msg( CYLF_MIDDLEWARE, CY_LOG_INFO, "\nCreating clean session ..\n" );
    }

    cy_mqtt_log_msg( CYLF_MIDDLEWARE, CY_LOG_INFO, "\nCreating an MQTT connection to %.*s.",
                     strlen(mqtt_obj->server_info.host_name), mqtt_obj->server_info.host_name );

    /* Sends an MQTT Connect packet using the established TLS session. */
    result = mqtt_establish_session( mqtt_obj, &connect_details, will_msg_ptr, create_clean_session, &(mqtt_obj->broker_session_present) );
    if( result != CY_RSLT_SUCCESS )
    {
        cy_mqtt_log_msg( CYLF_MIDDLEWARE, CY_LOG_ERR, "\nEstablish MQTT session failed with Error : [0x%X] ", (unsigned int)result );
        goto exit;
    }
    else
    {
        if( mqtt_obj->recv_thread == NULL )
        {
            char th_name[32];
            static uint8_t thread_sno = 0;

            snprintf( th_name, sizeof(th_name), "%d%s", ++thread_sno, " -MQTTReceive\n"  );
            cy_mqtt_log_msg( CYLF_MIDDLEWARE, CY_LOG_DEBUG, "\nCreating MQTT Receive thread......\n" );
            mqtt_obj->recv_thread = NULL;
            result = cy_rtos_create_thread( &mqtt_obj->recv_thread,
                                            mqtt_receive_thread,
                                            th_name,
                                            NULL,
                                            CY_MQTT_RECEIVE_THREAD_STACK_SIZE,
                                            CY_MQTT_RECEIVE_THREAD_PRIORITY,
                                            (cy_thread_arg_t)mqtt_obj );
            if( result != CY_RSLT_SUCCESS )
            {
                cy_mqtt_log_msg( CYLF_MIDDLEWARE, CY_LOG_ERR, "\nMQTT receive thread creation failed with Error : [0x%X] ", (unsigned int)result );
                goto exit;
            }
        }

        if( (mqtt_obj->broker_session_present == true) && (create_clean_session == false) )
        {
            cy_mqtt_log_msg( CYLF_MIDDLEWARE, CY_LOG_INFO, "\nMQTT session with broker is re-established. Resending unacked publishes." );
            /* Handle all resend of PUBLISH messages. */
            result = mqtt_handle_publish_resend( mqtt_obj );
            if( result != CY_RSLT_SUCCESS )
            {
                cy_mqtt_log_msg( CYLF_MIDDLEWARE, CY_LOG_ERR, "\nHandle all the resend of PUBLISH messages failed with Error : [0x%X] ", (unsigned int)result );
                goto exit;
            }
        }
        else
        {
            cy_mqtt_log_msg( CYLF_MIDDLEWARE, CY_LOG_INFO, "\n A clean MQTT connection is established. Cleaning up all the stored outgoing publishes." );

            /* Clean up the outgoing PUBLISH packets and wait for ack because this new
             * connection does not re-establish an existing session. */
            result = mqtt_cleanup_outgoing_publishes( mqtt_obj );
            if( result != CY_RSLT_SUCCESS )
            {
                cy_mqtt_log_msg( CYLF_MIDDLEWARE, CY_LOG_ERR, "\nCleaning of PUBLISH messages failed with Error : [0x%X] ", (unsigned int)result );
                goto exit;
            }
        }
    }

    mqtt_obj->mqtt_conn_status = true;
    return result;

exit :

    if( mqtt_obj->mqtt_session_established == true )
    {
        mqttStatus = MQTT_Disconnect( &(mqtt_obj->mqtt_context) );
        if( mqttStatus != MQTTSuccess )
        {
            cy_mqtt_log_msg( CYLF_MIDDLEWARE, CY_LOG_ERR, "Sending MQTT DISCONNECT failed with status=%s.",
                             MQTT_Status_strerror( mqttStatus ) );
            /*
             * In case of an unexpected network disconnection, the MQTT_Disconnect API always returns failure. Therefore,
             * the return value of the MQTT_Disconnect API is not checked here.
             */
            /* Fall-through. */
        }
        mqtt_obj->mqtt_session_established = false;
    }

    if( mqtt_obj->recv_thread != NULL )
    {
        cy_mqtt_log_msg( CYLF_MIDDLEWARE, CY_LOG_DEBUG, "\nTerminating MQTT receive thread %p..!\n", mqtt_obj->recv_thread );
        res = cy_rtos_terminate_thread( &mqtt_obj->recv_thread  );
        if( res != CY_RSLT_SUCCESS )
        {
            cy_mqtt_log_msg( CYLF_MIDDLEWARE, CY_LOG_ERR, "\nTerminate MQTT receive thread failed with Error : [0x%X] ", (unsigned int)result );
            /*
             * In case of an unexpected thread failure, the cy_rtos_terminate_thread API always returns failure. Therefore,
             * the return value of the cy_rtos_terminate_thread API is not checked here.
             */
            /* Fall-through. */
        }

        cy_mqtt_log_msg( CYLF_MIDDLEWARE, CY_LOG_DEBUG, "\nJoining MQTT receive thread %p..!\n", mqtt_obj->recv_thread );
        res = cy_rtos_join_thread( &mqtt_obj->recv_thread );
        if( res != CY_RSLT_SUCCESS )
        {
            cy_mqtt_log_msg( CYLF_MIDDLEWARE, CY_LOG_ERR, "\nJoin MQTT receive thread failed with Error : [0x%X] ", (unsigned int)result );
            /*
             * In case of an unexpected thread failure, the cy_rtos_join_thread API always returns failure. Therefore,
             * the return value of the cy_rtos_join_thread API is not checked here.
             */
            /* Fall-through. */
        }
        mqtt_obj->recv_thread = NULL;
    }

    res = cy_awsport_network_disconnect( &(mqtt_obj->network_context) );
    if( res != CY_RSLT_SUCCESS )
    {
        cy_mqtt_log_msg( CYLF_MIDDLEWARE, CY_LOG_ERR, "\ncy_awsport_network_disconnect failed with Error : [0x%X] ", (unsigned int)result );
        /*
         * In case of an unexpected network disconnection, the cy_awsport_network_disconnect API always returns failure. Therefore,
         * the return value of the cy_awsport_network_disconnect API is not checked here.
         */
        /* Fall-through. */
    }
    res = cy_awsport_network_delete( &(mqtt_obj->network_context) );
    if( res != CY_RSLT_SUCCESS )
    {
        cy_mqtt_log_msg( CYLF_MIDDLEWARE, CY_LOG_ERR, "\ncy_awsport_network_delete failed with Error : [0x%X] ", (unsigned int)result );
        /*
         * In case of an unexpected network disconnection, the cy_awsport_network_delete API always returns failure. Therefore,
         * the return value of the cy_awsport_network_delete API is not checked here.
         */
        /* Fall-through. */
    }

    return result;
}

/*----------------------------------------------------------------------------------------------------------*/

cy_rslt_t cy_mqtt_publish( cy_mqtt_t mqtt_handle, cy_mqtt_publish_info_t *pubmsg )
{
    cy_rslt_t        result = CY_RSLT_SUCCESS;
    MQTTStatus_t     mqttStatus = MQTTSuccess;
    uint8_t          publishIndex = CY_MQTT_MAX_OUTGOING_PUBLISHES;
    cy_mqtt_object_t *mqtt_obj;
    uint8_t          retry = 0;
    uint32_t         timeout = 0;

    if( (mqtt_handle == NULL) || (pubmsg == NULL) )
    {
        cy_mqtt_log_msg( CYLF_MIDDLEWARE, CY_LOG_ERR, "\nBad arguments to cy_mqtt_publish()..!\n" );
        return CY_RSLT_MODULE_MQTT_BADARG;
    }

    mqtt_obj = (cy_mqtt_object_t *)mqtt_handle;
    if( mqtt_obj->mqtt_obj_initialized == false )
    {
        cy_mqtt_log_msg( CYLF_MIDDLEWARE, CY_LOG_ERR, "\nInvalid MQTT object..!\n" );
        return CY_RSLT_MODULE_MQTT_OBJ_NOT_INITIALIZED;
    }

    if( mqtt_obj->mqtt_session_established == false )
    {
        cy_mqtt_log_msg( CYLF_MIDDLEWARE, CY_LOG_ERR, "\nMQTT client session not present..!\n" );
        return CY_RSLT_MODULE_MQTT_NOT_CONNECTED;
    }

    /* Get the next free index for the outgoing PUBLISH packets. All QoS2 outgoing
     * PUBLISH packets are stored until a PUBREC is received. These messages are
     * stored for supporting a resend if a network connection is broken before
     * receiving a PUBREC. */
    result = mqtt_get_next_free_index_for_publish( mqtt_obj, &publishIndex );
    if( result != CY_RSLT_SUCCESS )
    {
        cy_mqtt_log_msg( CYLF_MIDDLEWARE, CY_LOG_ERR, "\nUnable to find a free spot for outgoing PUBLISH message.\n" );
        return CY_RSLT_MODULE_MQTT_PUBLISH_FAIL;
    }
    else
    {
        if( (pubmsg->qos == CY_MQTT_QOS0) || (pubmsg->qos == CY_MQTT_QOS1) || (pubmsg->qos == CY_MQTT_QOS2) )
        {
            mqtt_obj->outgoing_pub_packets[ publishIndex ].pubinfo.qos = (MQTTQoS_t)pubmsg->qos;
        }
        else
        {
            cy_mqtt_log_msg( CYLF_MIDDLEWARE, CY_LOG_ERR, "\nQoS level not supported..!\n" );
            return CY_RSLT_MODULE_MQTT_PUBLISH_FAIL;
        }
        mqtt_obj->outgoing_pub_packets[ publishIndex ].pubinfo.pTopicName = pubmsg->topic;
        mqtt_obj->outgoing_pub_packets[ publishIndex ].pubinfo.topicNameLength = pubmsg->topic_len;
        mqtt_obj->outgoing_pub_packets[ publishIndex ].pubinfo.pPayload = pubmsg->payload;
        mqtt_obj->outgoing_pub_packets[ publishIndex ].pubinfo.payloadLength = pubmsg->payload_len;

        result = cy_rtos_get_mutex( &(mqtt_obj->process_mutex), CY_RTOS_NEVER_TIMEOUT );
        if( result != CY_RSLT_SUCCESS )
        {
            cy_mqtt_log_msg( CYLF_MIDDLEWARE, CY_LOG_ERR, "\ncy_rtos_get_mutex for Mutex %p failed with Error : [0x%X] ", mqtt_obj->process_mutex, (unsigned int)result );
            return result;
        }
        cy_mqtt_log_msg( CYLF_MIDDLEWARE, CY_LOG_DEBUG, "\ncy_mqtt_publish - Acquired Mutex %p ", mqtt_obj->process_mutex );

        /* Get a new packet ID. */
        mqtt_obj->outgoing_pub_packets[ publishIndex ].packetid = MQTT_GetPacketId( &(mqtt_obj->mqtt_context) );
        mqtt_obj->pub_ack_status.packetid = mqtt_obj->outgoing_pub_packets[ publishIndex ].packetid;
        /* Publish retry loop. */
        do
        {
            mqtt_obj->pub_ack_status.puback_status = false;
            timeout = CY_MQTT_ACK_RECEIVE_TIMEOUT_MS;

            /* Send the PUBLISH packet. */
            mqttStatus = MQTT_Publish( &(mqtt_obj->mqtt_context),
                                       &(mqtt_obj->outgoing_pub_packets[ publishIndex ].pubinfo),
                                       mqtt_obj->outgoing_pub_packets[ publishIndex ].packetid );
            if( mqttStatus != MQTTSuccess )
            {
                cy_mqtt_log_msg( CYLF_MIDDLEWARE, CY_LOG_ERR, "Failed to send PUBLISH packet to broker with error = %s.",
                                 MQTT_Status_strerror( mqttStatus ) );
                result = CY_RSLT_MODULE_MQTT_PUBLISH_FAIL;
            }
            else
            {
                cy_mqtt_log_msg( CYLF_MIDDLEWARE, CY_LOG_DEBUG, "\nPUBLISH sent for topic %.*s to broker with packet ID %u.\n",
                                 pubmsg->topic_len, pubmsg->topic, mqtt_obj->outgoing_pub_packets[ publishIndex ].packetid );
                /* Process the incoming packet from the broker.
                 * Acknowledgment for PUBLISH ( PUBACK ) will be received here. */
                if( mqtt_obj->outgoing_pub_packets[ publishIndex ].pubinfo.qos != MQTTQoS0 )
                {
                    do
                    {
                        mqttStatus = MQTT_ProcessLoop( &(mqtt_obj->mqtt_context), CY_MQTT_RECEIVE_DATA_TIMEOUT_MS );
                        if( mqttStatus != MQTTSuccess )
                        {
                            cy_mqtt_log_msg( CYLF_MIDDLEWARE, CY_LOG_ERR, "\nMQTT_ProcessLoop returned with status = %s.", MQTT_Status_strerror( mqttStatus ) );
                            result = CY_RSLT_MODULE_MQTT_PUBLISH_FAIL;
                            break;
                        }
                        else
                        {
                            if( mqtt_obj->pub_ack_status.puback_status == true )
                            {
                                result = CY_RSLT_SUCCESS;
                                break;
                            }
                        }

                        timeout = timeout - CY_MQTT_SOCKET_RECEIVE_TIMEOUT_MS;

                    } while( timeout > 0 );

                    /* Assign the MQTT Status to an error in case of PUBACK/PUBREC receive failure to retry publish. */
                    if( mqtt_obj->pub_ack_status.puback_status == false )
                    {
                        result = CY_RSLT_MODULE_MQTT_PUBLISH_FAIL;
                        mqttStatus = MQTTRecvFailed;
                    }
                }
                else
                {
                    result = CY_RSLT_SUCCESS;
                }
                mqtt_obj->outgoing_pub_packets[ publishIndex ].pubinfo.dup = true;
            }
            retry++;
        } while( (mqttStatus != MQTTSuccess) && (retry < CY_MQTT_MAX_RETRY_VALUE) );

        if( result != CY_RSLT_SUCCESS )
        {
            cy_mqtt_log_msg( CYLF_MIDDLEWARE, CY_LOG_ERR, "\nFailed to send PUBLISH packet to broker with max retry..!\n " );
            mqtt_cleanup_outgoing_publish( mqtt_obj, publishIndex );
            (void)cy_rtos_set_mutex( &(mqtt_obj->process_mutex) );
            return result;
        }

        if( mqtt_obj->outgoing_pub_packets[ publishIndex ].pubinfo.qos == MQTTQoS0 )
        {
            /* Clean up outgoing_pub_packets for QoS0 PUBLISH packets.*/
            (void)mqtt_cleanup_outgoing_publish( mqtt_obj, publishIndex );
        }

        result = cy_rtos_set_mutex( &(mqtt_obj->process_mutex) );
        if( result != CY_RSLT_SUCCESS )
        {
            cy_mqtt_log_msg( CYLF_MIDDLEWARE, CY_LOG_ERR, "\ncy_rtos_set_mutex for Mutex %p failed with Error : [0x%X] ", (unsigned int)result );
            return result;
        }
        cy_mqtt_log_msg( CYLF_MIDDLEWARE, CY_LOG_DEBUG, "\ncy_mqtt_publish - Released Mutex %p ", mqtt_obj->process_mutex );
    }

    return result;
}

/*----------------------------------------------------------------------------------------------------------*/

cy_rslt_t cy_mqtt_subscribe( cy_mqtt_t mqtt_handle, cy_mqtt_subscribe_info_t *sub_info, uint8_t sub_count  )
{
    cy_rslt_t              result = CY_RSLT_SUCCESS;
    MQTTStatus_t           mqttStatus;
    cy_mqtt_object_t       *mqtt_obj;
    uint8_t                index = 0, retry = 0;
    MQTTSubscribeInfo_t    *sub_list = NULL;
    uint32_t               timeout = 0;

    if( (mqtt_handle == NULL) || (sub_info == NULL) || (sub_count < 1) || (sub_count > CY_MQTT_MAX_OUTGOING_SUBSCRIBES) )
    {
        cy_mqtt_log_msg( CYLF_MIDDLEWARE, CY_LOG_ERR, "\nBad arguments to cy_mqtt_subscribe()..!\n" );
        return CY_RSLT_MODULE_MQTT_BADARG;
    }

    mqtt_obj = (cy_mqtt_object_t *)mqtt_handle;
    if( mqtt_obj->mqtt_obj_initialized == false )
    {
        cy_mqtt_log_msg( CYLF_MIDDLEWARE, CY_LOG_ERR, "\nInvalid MQTT object..!\n" );
        return CY_RSLT_MODULE_MQTT_OBJ_NOT_INITIALIZED;
    }

    if( mqtt_obj->mqtt_session_established == false )
    {
        cy_mqtt_log_msg( CYLF_MIDDLEWARE, CY_LOG_ERR, "\nMQTT client session not present..!\n" );
        return CY_RSLT_MODULE_MQTT_NOT_CONNECTED;
    }

    if( sub_count > CY_MQTT_MAX_OUTGOING_SUBSCRIBES )
    {
        cy_mqtt_log_msg( CYLF_MIDDLEWARE, CY_LOG_ERR, "\nMax number of supported subscription count in single request is %d\n", (int)CY_MQTT_MAX_OUTGOING_SUBSCRIBES );
        return CY_RSLT_MODULE_MQTT_SUBSCRIBE_FAIL;
    }

    sub_list = (MQTTSubscribeInfo_t *)malloc( (sizeof(MQTTSubscribeInfo_t) * sub_count) );
    if( sub_list == NULL )
    {
        cy_mqtt_log_msg( CYLF_MIDDLEWARE, CY_LOG_ERR, "\nMemory not available to create sub_list..!\n" );
        return CY_RSLT_MODULE_MQTT_NOMEM;
    }
    cy_mqtt_log_msg( CYLF_MIDDLEWARE, CY_LOG_DEBUG, "\nsub_list : %p..!\n", sub_list );

    for( index = 0; index < sub_count; index++ )
    {
        if( sub_info[index].qos == CY_MQTT_QOS0 )
        {
            sub_list[ index ].qos = MQTTQoS0;
        }
        else if( sub_info[index].qos == CY_MQTT_QOS1 )
        {
            sub_list[ index ].qos = MQTTQoS1;
        }
        else if( sub_info[index].qos == CY_MQTT_QOS2 )
        {
            sub_list[ index ].qos = MQTTQoS2;
        }
        else
        {
            cy_mqtt_log_msg( CYLF_MIDDLEWARE, CY_LOG_ERR, "\nQoS not supported..!\n" );
            free( sub_list );
            return CY_RSLT_MODULE_MQTT_SUBSCRIBE_FAIL;
        }
        sub_info[ index ].allocated_qos = CY_MQTT_QOS_INVALID;
        sub_list[ index ].pTopicFilter = sub_info[index].topic;
        sub_list[ index ].topicFilterLength = sub_info[index].topic_len;
    }

    result = cy_rtos_get_mutex( &(mqtt_obj->process_mutex), CY_RTOS_NEVER_TIMEOUT );
    if( result != CY_RSLT_SUCCESS )
    {
        cy_mqtt_log_msg( CYLF_MIDDLEWARE, CY_LOG_ERR, "\ncy_rtos_get_mutex for Mutex %p failed with Error : [0x%X] ", mqtt_obj->process_mutex, (unsigned int)result );
        free( sub_list );
        return result;
    }

    /* Generate the packet identifier for the SUBSCRIBE packet. */
    mqtt_obj->sent_packet_id = MQTT_GetPacketId( &(mqtt_obj->mqtt_context) );
    cy_mqtt_log_msg( CYLF_MIDDLEWARE, CY_LOG_DEBUG, "\ncy_mqtt_subscribe - Acquired Mutex %p ", mqtt_obj->process_mutex );

    do
    {
        timeout = CY_MQTT_ACK_RECEIVE_TIMEOUT_MS;
        result = CY_RSLT_MODULE_MQTT_SUBSCRIBE_FAIL;
        memset( &mqtt_obj->sub_ack_status, 0x00, sizeof(mqtt_obj->sub_ack_status) );

        /*
         * num_of_subs_in_req is initialized with number of subscribe messages in one MQTT subscribe request.
         * Once after receiving the subscription acknowledgment this variable is set to zero.
         * So num_of_subs_in_req == 0 refers that there is no outstanding subscription messages waiting for acknowledgment. */
        mqtt_obj->num_of_subs_in_req = sub_count;

        /* Send the SUBSCRIBE packet. */
        mqttStatus = MQTT_Subscribe( &(mqtt_obj->mqtt_context),
                                     sub_list,
                                     sub_count,
                                     mqtt_obj->sent_packet_id );
        if( mqttStatus != MQTTSuccess )
        {
            cy_mqtt_log_msg( CYLF_MIDDLEWARE, CY_LOG_ERR, "\nFailed to send SUBSCRIBE packet to broker with error = %s.",
                             MQTT_Status_strerror( mqttStatus ) );
            result = CY_RSLT_MODULE_MQTT_SUBSCRIBE_FAIL;
        }
        else
        {
            for( index = 0; index < sub_count; index++ )
            {
                mqtt_obj->sub_ack_status[index] = MQTTSubAckFailure;
                cy_mqtt_log_msg( CYLF_MIDDLEWARE, CY_LOG_DEBUG, "\nSUBSCRIBE sent for topic %.*s to broker.\n",
                                 sub_list[ index ].topicFilterLength,
                                 sub_list[ index ].pTopicFilter );
            }
            do
            {
                /* Process the incoming packet from the broker.
                 * Acknowledgment for subscription ( SUBACK ) will be received here. */
                mqttStatus = MQTT_ProcessLoop( &(mqtt_obj->mqtt_context), CY_MQTT_RECEIVE_DATA_TIMEOUT_MS );
                if( mqttStatus != MQTTSuccess )
                {
                    cy_mqtt_log_msg( CYLF_MIDDLEWARE, CY_LOG_ERR, "\nMQTT_ProcessLoop returned with status = %s.", MQTT_Status_strerror( mqttStatus ) );
                    result = CY_RSLT_MODULE_MQTT_SUBSCRIBE_FAIL;
                    break;
                }

                /* if suback status is updated then num_of_subs_in_req will be set to 0 in mqtt_event_callback.*/
                if( mqtt_obj->num_of_subs_in_req == 0 )
                {
                    result = CY_RSLT_MODULE_MQTT_SUBSCRIBE_FAIL; /* Initialize result with failure. */
                    for( index = 0; index < sub_count; index++ )
                    {
                        if( mqtt_obj->sub_ack_status[index] == MQTTSubAckFailure )
                        {
                            cy_mqtt_log_msg( CYLF_MIDDLEWARE, CY_LOG_INFO, "\nMQTT broker rejected SUBSCRIBE request for topic %.*s .\n",
                                             sub_list[ index ].topicFilterLength,
                                             sub_list[ index ].pTopicFilter );
                            sub_info[ index ].allocated_qos = CY_MQTT_QOS_INVALID;
                        }
                        else
                        {
                            cy_mqtt_log_msg( CYLF_MIDDLEWARE, CY_LOG_INFO, "\nSUBSCRIBE accepted for topic %.*s with QoS %d .\n",
                                             sub_list[ index ].topicFilterLength,
                                             sub_list[ index ].pTopicFilter, mqtt_obj->sub_ack_status[index] );
                            if( mqtt_obj->sub_ack_status[index] == MQTTSubAckSuccessQos0 )
                            {
                                sub_info[ index ].allocated_qos = CY_MQTT_QOS0;
                            }
                            else if( mqtt_obj->sub_ack_status[index] == MQTTSubAckSuccessQos1 )
                            {
                                sub_info[ index ].allocated_qos = CY_MQTT_QOS1;
                            }
                            else
                            {
                                sub_info[ index ].allocated_qos = CY_MQTT_QOS2;
                            }
                            result = CY_RSLT_SUCCESS; /* Update with success if at least one subscription is successful. */
                        }
                    }
                    break; /* Received the ack. So exit timeout do loop */
                }
                timeout = timeout - CY_MQTT_SOCKET_RECEIVE_TIMEOUT_MS;
            } while( timeout > 0 );

            if( mqtt_obj->num_of_subs_in_req != 0 )
            {
                result = CY_RSLT_MODULE_MQTT_SUBSCRIBE_FAIL;
                mqttStatus = MQTTRecvFailed; /* Assign error value to retry subscribe. */
            }
        }
        retry++;
    } while( (mqttStatus != MQTTSuccess) && (retry < CY_MQTT_MAX_RETRY_VALUE) );

    if( result != CY_RSLT_SUCCESS )
    {
        cy_mqtt_log_msg( CYLF_MIDDLEWARE, CY_LOG_INFO, "\nSubscription ack status is MQTTSubAckFailure..!\n" );
        goto exit;
    }

    result = cy_rtos_set_mutex( &(mqtt_obj->process_mutex) );
    if( result != CY_RSLT_SUCCESS )
    {
        cy_mqtt_log_msg( CYLF_MIDDLEWARE, CY_LOG_ERR, "\ncy_rtos_set_mutex for Mutex %p failed with Error : [0x%X] ", (unsigned int)result );
        free( sub_list );
        return result;
    }

    cy_mqtt_log_msg( CYLF_MIDDLEWARE, CY_LOG_DEBUG, "\ncy_mqtt_subscribe - Released Mutex %p ", mqtt_obj->process_mutex );
    free( sub_list );
    return CY_RSLT_SUCCESS;

exit :
    (void)cy_rtos_set_mutex( &(mqtt_obj->process_mutex) );
    /* Free sub_list */
    if( sub_list != NULL )
    {
        free( sub_list );
    }

    return result;
}

/*----------------------------------------------------------------------------------------------------------*/

cy_rslt_t cy_mqtt_unsubscribe( cy_mqtt_t mqtt_handle, cy_mqtt_unsubscribe_info_t *unsub_info, uint8_t unsub_count )
{
    cy_rslt_t              result = CY_RSLT_SUCCESS;
    cy_mqtt_object_t       *mqtt_obj;
    MQTTStatus_t           mqttStatus;
    uint8_t                index = 0, retry = 0;
    MQTTSubscribeInfo_t    *unsub_list = NULL;
    uint32_t               timeout = 0;

    if( (mqtt_handle == NULL) || (unsub_info == NULL) || (unsub_count < 1) )
    {
        cy_mqtt_log_msg( CYLF_MIDDLEWARE, CY_LOG_ERR, "\nBad arguments to cy_mqtt_unsubscribe()..!\n" );
        return CY_RSLT_MODULE_MQTT_BADARG;
    }

    mqtt_obj = (cy_mqtt_object_t *)mqtt_handle;
    if( mqtt_obj->mqtt_obj_initialized == false )
    {
        cy_mqtt_log_msg( CYLF_MIDDLEWARE, CY_LOG_ERR, "\nInvalid MQTT object..!\n" );
        return CY_RSLT_MODULE_MQTT_OBJ_NOT_INITIALIZED;
    }

    if( mqtt_obj->mqtt_session_established == false )
    {
        cy_mqtt_log_msg( CYLF_MIDDLEWARE, CY_LOG_ERR, "\nMQTT client session not present..!\n" );
        return CY_RSLT_MODULE_MQTT_NOT_CONNECTED;
    }

    if( unsub_count > CY_MQTT_MAX_OUTGOING_SUBSCRIBES )
    {
        cy_mqtt_log_msg( CYLF_MIDDLEWARE, CY_LOG_ERR, "\nMax number of supported unsubscription count in single request is %d\n", (int)CY_MQTT_MAX_OUTGOING_SUBSCRIBES );
        return CY_RSLT_MODULE_MQTT_UNSUBSCRIBE_FAIL;
    }

    unsub_list = (MQTTSubscribeInfo_t *)malloc( (sizeof(MQTTSubscribeInfo_t) * unsub_count) );
    if( unsub_list == NULL )
    {
        cy_mqtt_log_msg( CYLF_MIDDLEWARE, CY_LOG_ERR, "\nMemory not available to create unsub_list..!\n" );
        return CY_RSLT_MODULE_MQTT_NOMEM;
    }
    cy_mqtt_log_msg( CYLF_MIDDLEWARE, CY_LOG_DEBUG, "\nsub_list : %p..!\n", unsub_list );

    for( index = 0; index < unsub_count; index++ )
    {
        if( unsub_info[index].qos == CY_MQTT_QOS0 )
        {
            unsub_list[ index ].qos = MQTTQoS0;
        }
        else if( unsub_info[index].qos == CY_MQTT_QOS1 )
        {
            unsub_list[ index ].qos = MQTTQoS1;
        }
        else if( unsub_info[index].qos == CY_MQTT_QOS2 )
        {
            unsub_list[ index ].qos = MQTTQoS2;
        }
        else
        {
            cy_mqtt_log_msg( CYLF_MIDDLEWARE, CY_LOG_ERR, "\nQoS level not supported...\n" );
            free( unsub_list );
            return CY_RSLT_MODULE_MQTT_UNSUBSCRIBE_FAIL;
        }
        unsub_list[ index ].pTopicFilter = unsub_info[index].topic;
        unsub_list[ index ].topicFilterLength = unsub_info[index].topic_len;
    }

    result = cy_rtos_get_mutex( &(mqtt_obj->process_mutex), CY_RTOS_NEVER_TIMEOUT );
    if( result != CY_RSLT_SUCCESS )
    {
        cy_mqtt_log_msg( CYLF_MIDDLEWARE, CY_LOG_ERR, "\ncy_rtos_get_mutex for Mutex %p failed with Error : [0x%X] ", mqtt_obj->process_mutex, (unsigned int)result );
        free( unsub_list );
        return result;
    }
    cy_mqtt_log_msg( CYLF_MIDDLEWARE, CY_LOG_DEBUG, "\ncy_mqtt_unsubscribe - Acquired Mutex %p ", mqtt_obj->process_mutex );

    /* Generate the packet identifier for the UNSUBSCRIBE packet. */
    mqtt_obj->sent_packet_id = MQTT_GetPacketId( &(mqtt_obj->mqtt_context) );

    do
    {
        timeout = CY_MQTT_ACK_RECEIVE_TIMEOUT_MS;
        mqtt_obj->unsub_ack_received = false;
        cy_mqtt_log_msg( CYLF_MIDDLEWARE, CY_LOG_INFO, "UNSUBSCRIBE sent for topic %.*s to broker.\n\n", unsub_info->topic_len, unsub_info->topic );
        /* Send the UNSUBSCRIBE packet. */
        mqttStatus = MQTT_Unsubscribe( &(mqtt_obj->mqtt_context),
                                       unsub_list,
                                       unsub_count,
                                       mqtt_obj->sent_packet_id );
        if( mqttStatus != MQTTSuccess )
        {
            cy_mqtt_log_msg( CYLF_MIDDLEWARE, CY_LOG_ERR, "Failed to send UNSUBSCRIBE packet to broker with error = %s.",
                        MQTT_Status_strerror( mqttStatus ) );
            result = CY_RSLT_MODULE_MQTT_UNSUBSCRIBE_FAIL;
        }
        else
        {
            do
            {
                /* Process the  incoming packet from the broker.
                 * Acknowledgment for UNSUBSCRIBE ( UNSUBACK ) will be received here. */
                mqttStatus = MQTT_ProcessLoop( &(mqtt_obj->mqtt_context), CY_MQTT_RECEIVE_DATA_TIMEOUT_MS );
                if( mqttStatus != MQTTSuccess )
                {
                    cy_mqtt_log_msg( CYLF_MIDDLEWARE, CY_LOG_ERR, "\nMQTT_ProcessLoop returned with status = %s.", MQTT_Status_strerror( mqttStatus ) );
                    result = CY_RSLT_MODULE_MQTT_UNSUBSCRIBE_FAIL;
                    break;
                }
                if( mqtt_obj->unsub_ack_received == true )
                {
                    result = CY_RSLT_SUCCESS;
                    break;
                }
                timeout = timeout - CY_MQTT_SOCKET_RECEIVE_TIMEOUT_MS;
            } while( timeout > 0 );

            if( mqtt_obj->unsub_ack_received == false )
            {
                cy_mqtt_log_msg( CYLF_MIDDLEWARE, CY_LOG_ERR, "\nNot received unsuback before timeout %u millisecond ", (unsigned int)CY_MQTT_ACK_RECEIVE_TIMEOUT_MS );
                result = CY_RSLT_MODULE_MQTT_UNSUBSCRIBE_FAIL;
                mqttStatus = MQTTRecvFailed; /* Assign error value to retry subscribe. */
            }
        }
        retry++;
    } while( (mqttStatus != MQTTSuccess) && (retry < CY_MQTT_MAX_RETRY_VALUE) );

    if( result != CY_RSLT_SUCCESS )
    {
        cy_mqtt_log_msg( CYLF_MIDDLEWARE, CY_LOG_INFO, "\nSubscription ack status is MQTTSubAckFailure..!\n" );
        goto exit;
    }

    result = cy_rtos_set_mutex( &(mqtt_obj->process_mutex) );
    if( result != CY_RSLT_SUCCESS )
    {
        cy_mqtt_log_msg( CYLF_MIDDLEWARE, CY_LOG_ERR, "\ncy_rtos_set_mutex for Mutex %p failed with Error : [0x%X] ", (unsigned int)result );
        free( unsub_list );
        return result;
    }
    cy_mqtt_log_msg( CYLF_MIDDLEWARE, CY_LOG_DEBUG, "\ncy_mqtt_unsubscribe - Released Mutex %p ", mqtt_obj->process_mutex );

    /* Free unsub_list. */
    free( unsub_list );
    return CY_RSLT_SUCCESS;

exit :
    (void)cy_rtos_set_mutex( &(mqtt_obj->process_mutex) );
    /* Free unsub_list. */
    if( unsub_list != NULL )
    {
        free( unsub_list );
    }

    return result;
}

/*----------------------------------------------------------------------------------------------------------*/

cy_rslt_t cy_mqtt_disconnect( cy_mqtt_t mqtt_handle )
{
    cy_rslt_t         result = CY_RSLT_SUCCESS;
    cy_mqtt_object_t  *mqtt_obj;
    MQTTStatus_t mqttStatus = MQTTSuccess;

    if( mqtt_handle == NULL )
    {
        cy_mqtt_log_msg( CYLF_MIDDLEWARE, CY_LOG_ERR, "\nBad arguments to cy_mqtt_disconnect()..!\n" );
        return CY_RSLT_MODULE_MQTT_BADARG;
    }

    mqtt_obj = (cy_mqtt_object_t *)mqtt_handle;

    cy_mqtt_log_msg( CYLF_MIDDLEWARE, CY_LOG_DEBUG, "\ncy_mqtt_disconnect - Acquiring Mutex %p ", mqtt_obj->process_mutex );
    result = cy_rtos_get_mutex( &(mqtt_obj->process_mutex), CY_RTOS_NEVER_TIMEOUT );
    if( result != CY_RSLT_SUCCESS )
    {
        cy_mqtt_log_msg( CYLF_MIDDLEWARE, CY_LOG_ERR, "\ncy_rtos_get_mutex for Mutex %p failed with Error : [0x%X] ", mqtt_obj->process_mutex, (unsigned int)result );
        return result;
    }
    cy_mqtt_log_msg( CYLF_MIDDLEWARE, CY_LOG_DEBUG, "\ncy_mqtt_disconnect - Acquired Mutex %p ", mqtt_obj->process_mutex );

    if( mqtt_obj->mqtt_obj_initialized == false )
    {
        cy_mqtt_log_msg( CYLF_MIDDLEWARE, CY_LOG_ERR, "\nInvalid MQTT object..!\n" );
        return CY_RSLT_MODULE_MQTT_OBJ_NOT_INITIALIZED;
    }

    if( mqtt_obj->mqtt_conn_status == false )
    {
        cy_mqtt_log_msg( CYLF_MIDDLEWARE, CY_LOG_ERR, "\nMQTT client not connected..!\n" );
        return CY_RSLT_MODULE_MQTT_NOT_CONNECTED;
    }

    if( mqtt_obj->recv_thread != NULL )
    {
        cy_mqtt_log_msg( CYLF_MIDDLEWARE, CY_LOG_DEBUG, "\nTerminating MQTT receive thread %p..!\n", mqtt_obj->recv_thread );
        result = cy_rtos_terminate_thread( &mqtt_obj->recv_thread  );
        if( result != CY_RSLT_SUCCESS )
        {
            cy_mqtt_log_msg( CYLF_MIDDLEWARE, CY_LOG_ERR, "\nTerminate MQTT receive thread failed with Error : [0x%X] ", (unsigned int)result );
            return result;
        }

        cy_mqtt_log_msg( CYLF_MIDDLEWARE, CY_LOG_DEBUG, "\nJoining MQTT receive thread %p..!\n", mqtt_obj->recv_thread );
        result = cy_rtos_join_thread( &mqtt_obj->recv_thread );
        if( result != CY_RSLT_SUCCESS )
        {
            cy_mqtt_log_msg( CYLF_MIDDLEWARE, CY_LOG_ERR, "\nJoin MQTT receive thread failed with Error : [0x%X] ", (unsigned int)result );
            return result;
        }
        mqtt_obj->recv_thread = NULL;
    }

    /* Send DISCONNECT. */
    mqttStatus = MQTT_Disconnect( &(mqtt_obj->mqtt_context) );
    if( mqttStatus != MQTTSuccess )
    {
        cy_mqtt_log_msg( CYLF_MIDDLEWARE, CY_LOG_ERR, "Sending MQTT DISCONNECT failed with status=%s.",
                         MQTT_Status_strerror( mqttStatus ) );
        /*
         * In case of an unexpected network disconnection, the MQTT_Disconnect API always returns failure. Therefore,
         * the return value of the MQTT_Disconnect API is not checked here.
         */
        /* Fall-through. */
    }

    mqtt_obj->mqtt_session_established = false;
    result = cy_awsport_network_disconnect( &(mqtt_obj->network_context) );
    if( result != CY_RSLT_SUCCESS )
    {
        cy_mqtt_log_msg( CYLF_MIDDLEWARE, CY_LOG_ERR, "\ncy_awsport_network_disconnect failed with Error : [0x%X] ", (unsigned int)result );
        /*
         * In case of an unexpected network disconnection, the cy_awsport_network_disconnect API always returns failure. Therefore,
         * the return value of the cy_awsport_network_disconnect API is not checked here.
         */
        /* Fall-through. */
    }

    result = cy_awsport_network_delete( &(mqtt_obj->network_context) );
    if( result != CY_RSLT_SUCCESS )
    {
        cy_mqtt_log_msg( CYLF_MIDDLEWARE, CY_LOG_ERR, "\ncy_awsport_network_delete failed with Error : [0x%X] ", (unsigned int)result );
        /*
         * In case of an unexpected network disconnection, the cy_awsport_network_delete API always returns failure. Therefore,
         * the return value of the cy_awsport_network_delete API is not checked here.
         */
        /* Fall-through. */
    }
    mqtt_obj->mqtt_conn_status = false;

    cy_mqtt_log_msg( CYLF_MIDDLEWARE, CY_LOG_DEBUG, "\ncy_mqtt_disconnect - Releasing Mutex %p ", mqtt_obj->process_mutex );
    result = cy_rtos_set_mutex( &(mqtt_obj->process_mutex) );
    if( result != CY_RSLT_SUCCESS )
    {
        cy_mqtt_log_msg( CYLF_MIDDLEWARE, CY_LOG_ERR, "\ncy_rtos_set_mutex for Mutex %p failed with Error : [0x%X] ", (unsigned int)result );
        return result;
    }
    cy_mqtt_log_msg( CYLF_MIDDLEWARE, CY_LOG_DEBUG, "\ncy_mqtt_disconnect - Released Mutex %p ", mqtt_obj->process_mutex );

    return result;
}

/*----------------------------------------------------------------------------------------------------------*/

cy_rslt_t cy_mqtt_delete( cy_mqtt_t mqtt_handle )
{
    cy_rslt_t         result = CY_RSLT_SUCCESS;
    cy_mqtt_object_t  *mqtt_obj;

    if( mqtt_handle == NULL )
    {
        cy_mqtt_log_msg( CYLF_MIDDLEWARE, CY_LOG_ERR, "\nBad arguments to cy_mqtt_delete()..!\n" );
        return CY_RSLT_MODULE_MQTT_BADARG;
    }

    mqtt_obj = (cy_mqtt_object_t *)mqtt_handle;
    if( mqtt_obj->mqtt_obj_initialized == false )
    {
        cy_mqtt_log_msg( CYLF_MIDDLEWARE, CY_LOG_ERR, "\nInvalid MQTT object..!\n" );
        return CY_RSLT_MODULE_MQTT_OBJ_NOT_INITIALIZED;
    }

    (void)cy_rtos_deinit_mutex( &(mqtt_obj->process_mutex) );

    result = cy_rtos_get_mutex( &mqtt_db_mutex, CY_RTOS_NEVER_TIMEOUT );
    if( result != CY_RSLT_SUCCESS )
    {
        cy_mqtt_log_msg( CYLF_MIDDLEWARE, CY_LOG_ERR, "\ncy_rtos_get_mutex for Mutex %p failed with Error : [0x%X] ", mqtt_db_mutex, (unsigned int)result );
        return result;
    }
    cy_mqtt_log_msg( CYLF_MIDDLEWARE, CY_LOG_DEBUG, "\ncy_mqtt_delete - Acquired Mutex %p ", mqtt_db_mutex );

    /* Clear entry in THE MQTT object-mqtt context table. */
    mqtt_handle_database[mqtt_obj->mqtt_obj_index].mqtt_handle = NULL;
    mqtt_handle_database[mqtt_obj->mqtt_obj_index].mqtt_context = NULL;
    mqtt_handle_count--;

    result = cy_rtos_set_mutex( &mqtt_db_mutex );
    if( result != CY_RSLT_SUCCESS )
    {
        cy_mqtt_log_msg( CYLF_MIDDLEWARE, CY_LOG_ERR, "\ncy_rtos_set_mutex for Mutex %p failed with Error : [0x%X] ", mqtt_db_mutex, (unsigned int)result );
        return result;
    }
    cy_mqtt_log_msg( CYLF_MIDDLEWARE, CY_LOG_DEBUG, "\ncy_mqtt_delete - Released Mutex %p ", mqtt_db_mutex );

    /* Clear the MQTT handle info. */
    ( void ) memset( mqtt_obj, 0x00, sizeof( cy_mqtt_object_t ) );

    cy_mqtt_log_msg( CYLF_MIDDLEWARE, CY_LOG_DEBUG, "\n Free mqtt_obj : %p..!\n", mqtt_obj );
    free( mqtt_obj );
    mqtt_handle = NULL;

    return CY_RSLT_SUCCESS;
}

/*----------------------------------------------------------------------------------------------------------*/

cy_rslt_t cy_mqtt_deinit( void )
{
    cy_rslt_t result = CY_RSLT_SUCCESS;

    if( (mqtt_lib_init_status == false) || (mqtt_db_mutex_init_status == false) )
    {
        cy_mqtt_log_msg( CYLF_MIDDLEWARE, CY_LOG_ERR, "\nLibrary init is not done/Global mutex is not initialized..!\n " );
        return CY_RSLT_MODULE_MQTT_DEINIT_FAIL;
    }

    if( mqtt_handle_count != 0 )
    {
        cy_mqtt_log_msg( CYLF_MIDDLEWARE, CY_LOG_INFO, "\nMQTT library is deinit cannot be done. Number of MQTT client instance : [%d] \n", mqtt_handle_count );
        return result;
    }

    result = cy_rtos_deinit_mutex( &mqtt_db_mutex );
    if( result != CY_RSLT_SUCCESS )
    {
        cy_mqtt_log_msg( CYLF_MIDDLEWARE, CY_LOG_ERR, "\ncy_rtos_deinit_mutex failed with Error : [0x%X] ", (unsigned int)result );
        return result;
    }

    mqtt_db_mutex_init_status = false;

    result = cy_awsport_network_deinit();
    if( result != CY_RSLT_SUCCESS )
    {
        cy_mqtt_log_msg( CYLF_MIDDLEWARE, CY_LOG_ERR, "\ncy_awsport_network_deinit failed with Error : [0x%X] ", (unsigned int)result );
        return result;
    }
    else
    {
        cy_mqtt_log_msg( CYLF_MIDDLEWARE, CY_LOG_DEBUG, "\ncy_awsport_network_deinit successful." );
    }

    if( mqtt_disconnect_event_thread != NULL )
    {
        cy_mqtt_log_msg( CYLF_MIDDLEWARE, CY_LOG_DEBUG, "\nTerminating MQTT disconnect event thread %p..!\n", mqtt_disconnect_event_thread );
        result = cy_rtos_terminate_thread( &mqtt_disconnect_event_thread  );
        if( result != CY_RSLT_SUCCESS )
        {
            cy_mqtt_log_msg( CYLF_MIDDLEWARE, CY_LOG_ERR, "\nTerminate MQTT disconnect event thread failed with Error : [0x%X] ", (unsigned int)result );
            return result;
        }

        cy_mqtt_log_msg( CYLF_MIDDLEWARE, CY_LOG_DEBUG, "\nJoining MQTT disconnect event thread %p..!\n", mqtt_disconnect_event_thread );
        result = cy_rtos_join_thread( &mqtt_disconnect_event_thread );
        if( result != CY_RSLT_SUCCESS )
        {
            cy_mqtt_log_msg( CYLF_MIDDLEWARE, CY_LOG_ERR, "\nJoin MQTT disconnect event thread failed with Error : [0x%X] ", (unsigned int)result );
            return result;
        }
        mqtt_disconnect_event_thread = NULL;
    }

    result = cy_rtos_deinit_queue( &mqtt_disconnect_event_queue );
    if( result != CY_RSLT_SUCCESS )
    {
        cy_mqtt_log_msg( CYLF_MIDDLEWARE, CY_LOG_ERR, "\ncy_rtos_deinit_queue failed with Error : [0x%X] ", (unsigned int)result );
        return result;
    }
    else
    {
        cy_mqtt_log_msg( CYLF_MIDDLEWARE, CY_LOG_DEBUG, "\ncy_rtos_deinit_queue successful." );
    }

    mqtt_lib_init_status = false;
    return CY_RSLT_SUCCESS;
}

/*----------------------------------------------------------------------------------------------------------*/
