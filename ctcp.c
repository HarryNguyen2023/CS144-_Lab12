/******************************************************************************
 * ctcp.c
 * ------
 * Implementation of cTCP done here. This is the only file you need to change.
 * Look at the following files for references and useful functions:
 *   - ctcp.h: Headers for this file.
 *   - ctcp_iinked_list.h: Linked list functions for managing a linked list.
 *   - ctcp_sys.h: Connection-related structs and functions, cTCP segment
 *                 definition.
 *   - ctcp_utils.h: Checksum computation, getting the current time.
 *
 *****************************************************************************/

#include "ctcp.h"
#include "ctcp_linked_list.h"
#include "ctcp_sys.h"
#include "ctcp_utils.h"

/*
  * Flags for types of segment
  * DATA_SEG: data segment
  * ACK_SEG; Acknowledge segment
  * FIN_WITH_ACK: FIN + ACk from web servers
  * FIN_WITH_NO_ACK: FIN from client to terminate conneciton
*/
typedef enum Segment_type
{
  DATA_SEG,
  ACK_SEG,
  FIN_WITH_ACK,
  FIN_WITH_NO_ACK
}Segment_type;

/*
  * Flags for type of connection teardown
  * No CLOSE: normal operation
  * ACTIVE CLOSE: client active close
  * PASSIVE CLOSE: server passive close
*/
typedef enum Teardown_state
{
  NO_CLOSE,
  ACTIVE_CLOSE,
  PASSIVE_CLOSE
}Teardown_state;

/*
  * Store the information of the ACK of a connection
  * send_ack: flag for the receiver to wether send ACk or not
  * time_out: flag if segment need to time out
  * time_out_num: number to keep track of the number of time out already taken place
*/
typedef struct ACK_state
{
  uint8_t time_out_num;
  uint8_t counter;
  uint8_t timer_overflow;
  bool time_out;
}ACK_state;

/*
  * Store the information of the connection state
*/
typedef struct Conn_state
{
  uint32_t seqno;
  uint32_t next_seqno;
  uint32_t ackno;
  uint32_t last_ackno;
  uint16_t send_window;
  uint16_t send_window_used;
  uint16_t rcv_window;
  uint16_t rcv_window_used;
}Conn_state;

/*
  * Store the information of the transmit data
  * buffer size: size of the tx buffer
  * tx buffer: flexible array member
*/
typedef struct TX_state
{
  uint32_t segment_next_seqno;
  int buffer_size;
  char tx_buffer[];
}TX_state;

/*
  * Store information of the received data
  * rx_buffer: buffer to store received data
  * byte_used: byte sent to STDOUT
  * byte_left: byte not sent yet
*/
typedef struct RX_state
{
  int byte_used;
  int byte_left;
  char rx_buffer[];
}RX_state;

/**
 * Connection state.
 *
 * Stores per-connection information such as the current sequence number,
 * unacknowledged packets, etc.
 *
 * You should add to this to store other fields you might need.
 */
struct ctcp_state {
  struct ctcp_state *next;  /* Next in linked list */
  struct ctcp_state **prev; /* Prev in linked list */

  conn_t *conn;             /* Connection object -- needed in order to figure
                               out destination when sending */

  /* FIXME: Add other needed fields. */
  Conn_state conn_state;            // Connection state
  linked_list_t *tx_state;               // Transmit buffer state
  linked_list_t *rx_state;               // Receive buffer state
  ACK_state ack_state;              // Time out condition of the segment
  Teardown_state segment_teardown;  // Teardown state of the conneciton
};

/**
 * Linked list of connection states. Go through this in ctcp_timer() to
 * resubmit segments and tear down connections.
 */
static ctcp_state_t *state_list;

/* FIXME: Feel free to add as many helper functions as needed. Don't repeat
          code! Helper functions make the code clearer and cleaner. */

/******************************* Helper function prototypes *********************************/
static void ctcp_send_flags(ctcp_state_t *state, uint32_t ackno, uint32_t flags);
static void ctcp_receive_data_segment(ctcp_state_t *state, ctcp_segment_t *segment, size_t len);
static void ctcp_receive_fin_with_no_ack(ctcp_state_t *state, ctcp_segment_t *segment);
static void ctcp_send_data_segment(ctcp_state_t *state, ll_node_t *tx_state_node);
static void ctcp_send_possible_data_segment(ctcp_state_t *state);

ctcp_state_t *ctcp_init(conn_t *conn, ctcp_config_t *cfg) {
  /* Connection could not be established. */
  if (conn == NULL) {
    return NULL;
  }
  /* Established a connection. Create a new state and update the linked list
     of connection states. */
  ctcp_state_t *state = calloc(sizeof(ctcp_state_t), 1);
  state->next = state_list;
  state->prev = &state_list;
  if (state_list)
    state_list->prev = &state->next;
  state_list = state;
  /* Set fields. */
  state->conn = conn;
  // Set connection state
  state->conn_state.seqno = 1;
  state->conn_state.next_seqno = 1;
  state->conn_state.ackno = 1;
  state->conn_state.last_ackno = 1;
  state->conn_state.send_window = cfg->send_window;
  state->conn_state.send_window_used = 0;
  state->conn_state.rcv_window = cfg->recv_window;
  state->conn_state.rcv_window_used = 0;

  // Initiate the segment ACK
  state->ack_state.time_out = false;
  state->ack_state.time_out_num = 0;
  state->ack_state.counter = 0;
  state->ack_state.timer_overflow = ((cfg->rt_timeout % cfg->timer) == 0) ? (cfg->rt_timeout / cfg->timer) : (cfg->rt_timeout / cfg->timer) + 1;
  // Initiate the teardown condition
  state->segment_teardown = NO_CLOSE;
  // Allocate linked list of tx state & rx_state
  state->tx_state = ll_create();
  state->rx_state = ll_create();

  // Deallocate cfg pointer
  free(cfg);
  return state;
}

void ctcp_destroy(ctcp_state_t *state) {
  /* Update linked list. */
  if (state->next)
    state->next->prev = state->prev;

  *state->prev = state->next;
  conn_remove(state->conn);

  // Destroy the 2 linked list inside the state
  ll_destroy(state->tx_state);
  ll_destroy(state->rx_state);

  free(state);
  state = NULL;
  end_client();
}

/*
  * Function to send the data segment over connection
  * Param state: state of the current connection
  * Param data_buffer: data need to be transmitted over the connection
*/
static void ctcp_send_data_segment(ctcp_state_t *state, ll_node_t *tx_state_node)
{
  // Get the current head node of the linked list
  if(tx_state_node == NULL)
    return;
  // Initiate some variables
  int byte_sent = 0;
  int byte_left;
  // Create data segment to send over the conneciton
  ctcp_segment_t *data_segment = (ctcp_segment_t *)calloc(sizeof(ctcp_segment_t) + (sizeof(char) * ((TX_state*)(tx_state_node->object))->buffer_size), 1);
  if(data_segment == NULL)
    return;
  // Fill in the data segment
  data_segment->seqno = htonl(state->conn_state.next_seqno);
  data_segment->ackno = htonl(state->conn_state.ackno);
  // Update the next_seqno number if not retransmission
  state->conn_state.next_seqno += ((TX_state*)(tx_state_node->object))->buffer_size;
  ((TX_state*)(tx_state_node->object))->segment_next_seqno = state->conn_state.next_seqno;

  int data_seg_len = sizeof(ctcp_segment_t) + sizeof(char) * ((TX_state*)(tx_state_node->object))->buffer_size;
  data_segment->len = htons(data_seg_len);
  data_segment->flags = htonl(0);
  data_segment->window = htons(MAX_SEG_DATA_SIZE * ((state->conn_state.rcv_window - state->conn_state.rcv_window_used) / MAX_SEG_DATA_SIZE));
  // Initiate data buffer
  memcpy(data_segment->data, ((TX_state*)(tx_state_node->object))->tx_buffer, ((TX_state*)(tx_state_node->object))->buffer_size);
  // Checksum
  data_segment->cksum = 0;
  data_segment->cksum = cksum(data_segment, data_seg_len);
  byte_left = data_seg_len;
  // Send the data over the connection
  while(byte_left > 0)
  {
    byte_sent = conn_send(state->conn, data_segment + data_seg_len - byte_left, byte_left);
    byte_left -= byte_sent;
  }
  // Set time out flag 
  state->ack_state.time_out = true;
  free(data_segment);
}

/*
  @brief: Function to send all the possible sending sliding window over the conneciton using Go Back N technique
  @param state: state of the current connection
  @param tx_start_node: the start node contain data to be sent of the sliding window
  @return value: none
*/
static void ctcp_send_possible_data_segment(ctcp_state_t *state)
{
  // Initiate the sending window used of the connection 
  state->conn_state.send_window_used = 0;
  // Update the next_seqno number of the connection
  state->conn_state.next_seqno = state->conn_state.seqno;
  // Send data over the connetion
  ll_node_t* tx_state_node = ll_front(state->tx_state);
  // Send the whole sending window size
  while(tx_state_node != NULL)
  {
    // Check if we have send the whole sending window size
    if(((TX_state*)(tx_state_node->object))->buffer_size + state->conn_state.send_window_used > state->conn_state.send_window)
      break;
    // Send out the sending window of the data segment
    ctcp_send_data_segment(state, tx_state_node);
    // Update the used window size 
    state->conn_state.send_window_used += ((TX_state*)(tx_state_node->object))->buffer_size;
    // Move to the next segment
    tx_state_node = tx_state_node->next;
  }
}

void ctcp_read(ctcp_state_t *state) 
{
  int byte_read = 0;
  // Initiate the buffer for reading input from user
  size_t read_len = MAX_SEG_DATA_SIZE;
  char *tx_buffer = (char*)calloc(sizeof(char) * read_len, 1);

  // Read input from STDIN
  while((byte_read = conn_input(state->conn, tx_buffer, read_len)) >= -1)
  {
    if(byte_read == 0)
      break;
    // Case read EOF
    else if(byte_read == -1)
    {
      // Send out all of the data to STDOUT
      while(state->rx_state->length > 0);
      // Send all read data over the connection
      while(state->tx_state->length > 0);
      // Update the teardown state
      state->segment_teardown = ACTIVE_CLOSE;
      // Send FIN to close the socket
      ctcp_send_flags(state, state->conn_state.ackno, FIN);
      // Set time out flag 
      state->ack_state.time_out = true;
      break;
    }
    // Check if read truncated message
    if(byte_read > 14)
    {
      char truncated_buffer[15] = "\0";
      // Copy the first 14 bytes of the reading message 
      memcpy(truncated_buffer, tx_buffer, 14);
      truncated_buffer[14] = '\0';
      // Detect if truncated message
      if(strcmp(truncated_buffer, "###truncate###") == 0)
        break;
    }
    // Create the TX state object for the current segment
    TX_state *segemnt_tx = (TX_state*)calloc(sizeof(TX_state) + sizeof(char) * byte_read, 1);
    memcpy(segemnt_tx->tx_buffer, tx_buffer, byte_read);
    segemnt_tx->buffer_size = byte_read;
    
    // Add the new TX state to the linked list
    ll_add(state->tx_state, segemnt_tx);
  }
  // Deallocated the tx buffer
  free(tx_buffer);
  tx_buffer = NULL;
  // Send all possisble data segment of the sliding window
  ctcp_send_possible_data_segment(state);
}

/*
  * Function to send ACK segmnt
  * Param state: state of the current conneciton
  * Return value: none
*/
static void ctcp_send_flags(ctcp_state_t *state, uint32_t ackno, uint32_t flags)
{
  int byte_sent = 0, byte_left, segment_len; 
  segment_len = sizeof(ctcp_segment_t);
  byte_left = segment_len;

  // Fill the ACK segment
  ctcp_segment_t* ack_segment = calloc(1, sizeof(ctcp_segment_t));
  ack_segment->seqno = htonl(state->conn_state.seqno);
  ack_segment->ackno = htonl(ackno);
  ack_segment->len = htons(segment_len);
  ack_segment->flags |= htonl(flags);
  // ack_segment->window = htons(MAX_SEG_DATA_SIZE * ((state->conn_state.rcv_window - state->conn_state.rcv_window_used) / MAX_SEG_DATA_SIZE));
  ack_segment->window = htons(MAX_SEG_DATA_SIZE * ((state->conn_state.rcv_window - state->conn_state.rcv_window_used) / MAX_SEG_DATA_SIZE));
  // Get the checksum number of the segment
  ack_segment->cksum = 0;
  ack_segment->cksum = cksum(ack_segment, segment_len);

  // Send the ACK to the IP socket
  while(byte_left > 0)
  {
    byte_sent = conn_send(state->conn, (ack_segment + segment_len - byte_left), byte_left);
    byte_left -= byte_sent;
  }
  free(ack_segment);
}

/*
  * Function to handle the reception of data segment
  * Param state: state of the current conneciton
  * Param sgement: data segment received from socket
  * Param len: length of the received data segment
  * Return value: none
*/
static void ctcp_receive_data_segment(ctcp_state_t *state, ctcp_segment_t *segment, size_t len)
{
  // Get the actual data length
  int data_seg_len = len - sizeof(ctcp_segment_t);
  // Add new data segment into the receive sliding window
  if(state->conn_state.rcv_window_used + data_seg_len <= state->conn_state.rcv_window)
  {
    // Update the ACK number of the connection
    state->conn_state.last_ackno = state->conn_state.ackno;
    state->conn_state.ackno = ntohl(segment->seqno) + ntohs(segment->len) - sizeof(ctcp_segment_t);

    // Copy segment data into the node buffer
    RX_state* rx_state_node = (RX_state*)calloc(sizeof(RX_state) + sizeof(char) * data_seg_len, 1);
    memcpy(rx_state_node->rx_buffer, segment->data, data_seg_len);
    rx_state_node->byte_left = data_seg_len;
    rx_state_node->byte_used = 0;

    // Update the used received window size
    state->conn_state.rcv_window_used += data_seg_len;
    // Add segment node into the sliding window
    ll_add(state->rx_state, rx_state_node);
  }
  // Output data to STDOUT
  ctcp_output(state);
}

/*
  * Function to handle the reception of FIN 
  * Param state: state of the current connection
  * Param segment: received data segment
  * Return value: none
*/
static void ctcp_receive_fin_with_no_ack(ctcp_state_t *state, ctcp_segment_t *segment)
{
  // Update the ackno of the conenction
  state->conn_state.last_ackno = state->conn_state.ackno;
  state->conn_state.ackno = ntohl(segment->seqno) + 1;
  // Case server passive close
  if(state->segment_teardown != ACTIVE_CLOSE)
  {
    // Send EOF to STDOUT
    conn_output(state->conn, NULL, 0);
    // Send ACK after received FIN
    ctcp_send_flags(state, state->conn_state.ackno, ACK);
    // Send out all of the data to STDOUT
    while(state->rx_state->length > 0);
    // Send FIN back
    ctcp_send_flags(state, state->conn_state.ackno, FIN);
    // Raise timeout flag 
    state->ack_state.time_out = true;
    // Update the teardown state
    state->segment_teardown = PASSIVE_CLOSE;
  }
  // Case client receive the 2nd FIN
  else if(state->segment_teardown == ACTIVE_CLOSE)
  {
    // Send ACK after received FIN
    ctcp_send_flags(state, state->conn_state.ackno, ACK);
    // Close the connection
    ctcp_destroy(state);
  }
}

void ctcp_receive(ctcp_state_t *state, ctcp_segment_t *segment, size_t len) 
{
  // Verify duplicate data segment and resend ackno for the last segment
  if(ntohl(segment->seqno) != state->conn_state.ackno && ntohl(segment->seqno) == state->conn_state.last_ackno && (! (ntohl(segment->flags) & ACK)))
  {
    // Resend the last ACK segment
    ctcp_send_flags(state, state->conn_state.last_ackno, ACK);
    free(segment);
    return;
  }
  // Discard truncated received segment
  else if(len != ntohs(segment->len))
  {
    free(segment);
    return;
  }
  // Verify the checksum field of the data
  uint16_t segment_check_sum = segment->cksum;
  segment->cksum = 0;
  if(segment_check_sum != cksum(segment, len))
  {
    free(segment);
    return;
  }
  segment->cksum = segment_check_sum;

  // Intiiate some variables
  Segment_type cur_seg_type;

  // Check the type of received data
  if(ntohl(segment->flags) & FIN)
  {
    if(ntohl(segment->flags) & ACK)
      cur_seg_type = FIN_WITH_ACK;
    else
      cur_seg_type = FIN_WITH_NO_ACK;
  }
  else if(ntohl(segment->flags) & ACK)
    cur_seg_type = ACK_SEG;
  else
    cur_seg_type = DATA_SEG;
  
  // State machine for types of received segments
  switch(cur_seg_type)
  {
    case DATA_SEG:
    {
      ctcp_receive_data_segment(state, segment, len);
    }
    break;

    case ACK_SEG:
    {
      // Teardown the connection if this is the last ACK
      if(state->segment_teardown == PASSIVE_CLOSE)
      {
        ctcp_destroy(state);
        return; 
      }
      ll_node_t* tx_state_node = ll_front(state->tx_state);
      if(tx_state_node == NULL)
        return;
      uint32_t next_seqno = ((TX_state*)(tx_state_node->object))->segment_next_seqno;
      uint32_t segment_ackno = ntohl(segment->ackno);
      // Handle cummulative acknowledgement
      if(segment_ackno >= next_seqno)
      {
        while(segment_ackno >= ((TX_state*)(tx_state_node->object))->segment_next_seqno && ((TX_state*)(tx_state_node->object))->segment_next_seqno > 0)
        {
          // Update sequence number
          state->conn_state.seqno = ((TX_state*)(tx_state_node->object))->segment_next_seqno;
          // Update the used sending window size
          state->conn_state.send_window_used -= ((TX_state*)(tx_state_node->object))->buffer_size;
          // Deallocate the head of tx state
          free(tx_state_node->object);
          tx_state_node->object = NULL;
          // Move to the next node and delete the head node of the linked list
          if(tx_state_node->next != NULL)
            tx_state_node = tx_state_node->next;
          else
          {
            ll_remove(state->tx_state, ll_front(state->tx_state));
            break;
          }
          ll_remove(state->tx_state, ll_front(state->tx_state));
        }
        // Deactivate time out flag
        if(segment_ackno == state->conn_state.next_seqno)
          state->ack_state.time_out = false;
        // Reset the time out counter
        state->ack_state.counter = 0;
        state->ack_state.time_out_num = 0; 
      }
      
    }
    break;

    case FIN_WITH_ACK:
    {
      // Update the ackno number
      state->conn_state.ackno = ntohl(segment->seqno) + 1;
      // Send back the last ACK
      ctcp_send_flags(state, state->conn_state.ackno, ACK);
      // Teardown the conneciton
      ctcp_destroy(state);
    }
    break;

    case FIN_WITH_NO_ACK:
    {
      ctcp_receive_fin_with_no_ack(state, segment);
    }
    break;

    default:
    {
      free(segment);
      return;
    }
  }
  free(segment);
}

void ctcp_output(ctcp_state_t *state) {
  // Get the head of the receive sliding window
  ll_node_t* rx_state_node = ll_front(state->rx_state);
  if(rx_state_node == NULL)
    return;

  // Check if there is enough available space to output to STDOUT
  while(rx_state_node != NULL)
  {
    // Get the availabe space
    size_t avai_space = conn_bufspace(state->conn);
    if(! avai_space || ((RX_state*)(rx_state_node->object))->byte_left > avai_space)
      break;
    
    // Actually output the buffer to the STDOUT
    int byte_sent = conn_output(state->conn, (((RX_state*)(rx_state_node->object))->rx_buffer + ((RX_state*)(rx_state_node->object))->byte_used), ((RX_state*)(rx_state_node->object))->byte_left);
    // Update the RX state of the connection
    ((RX_state*)(rx_state_node->object))->byte_used += byte_sent;
    ((RX_state*)(rx_state_node->object))->byte_left -= byte_sent;
    // Update the receive window used
    state->conn_state.rcv_window_used -= byte_sent;

    // Flow control and deallocation of buffer
    if(((RX_state*)(rx_state_node->object))->byte_left <= 0)
    {
      // Send out ACK for the buffer
      ctcp_send_flags(state, state->conn_state.ackno, ACK);
      // Deallocate buffer for the rx state node
      free(rx_state_node->object);
      rx_state_node->object = NULL;
    }
    else
      break;

    rx_state_node = rx_state_node->next;
    // Delete the last node
    ll_remove(state->rx_state, ll_front(state->rx_state));
  }
}

void ctcp_timer() {
  // Verify the existence of state list 
  if(state_list == NULL)
    return;
  // Get the head of the state list
  ctcp_state_t *cur_state = state_list;
  // Traverse the state linked list
  while(cur_state != NULL)
  {
    // Check timeout condition
    if(cur_state->ack_state.time_out)
    {
      if(++(cur_state->ack_state.counter) == cur_state->ack_state.timer_overflow)
      {
        cur_state->ack_state.counter = 0;
        // Teardown connection at the 6th time out
        if(++(cur_state->ack_state.time_out_num) == 6)
        {
          // Send FIN
          ctcp_send_flags(cur_state, cur_state->conn_state.ackno, FIN);
          // Set time out for FIN
          cur_state->ack_state.time_out = true;
          // Update teardown state
          cur_state->segment_teardown = ACTIVE_CLOSE;

          continue;
        }
        // FIN segment timeout
        if(cur_state->segment_teardown == ACTIVE_CLOSE || cur_state->segment_teardown == PASSIVE_CLOSE) 
        {
          // Retransmit FIN segment
          ctcp_send_flags(cur_state, cur_state->conn_state.last_ackno, FIN);
        }
        else if(cur_state->segment_teardown == NO_CLOSE)
        {
          // Retrnasmit all the unacked data segment + new data segment of the sliding window
          ctcp_send_possible_data_segment(cur_state);
        }
      }
    }
    else 
    {
      // Send the left data segments
      ctcp_send_possible_data_segment(cur_state);
      // Send out of received data segment to STDOUT
      if(cur_state->rx_state->length > 0)
      {
        ctcp_output(cur_state);
      }
    }
    cur_state = cur_state->next;
  }
}
