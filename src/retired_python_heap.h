#pragma once
// Python 2.1 GC headers: next, prev, gc_refs. Do not inspect object/type pointers.
namespace retired_python {
struct Node { Node* next; Node* prev; long refs; };
inline void initialize(Node& head) { head.next=head.prev=&head; head.refs=0; }
inline void transfer(Node& source, Node& destination) {
    if (source.next==&source) return;
    Node* first=source.next; Node* last=source.prev; Node* tail=destination.prev;
    tail->next=first; first->prev=tail;
    last->next=&destination; destination.prev=last;
    source.next=source.prev=&source;
}
}
