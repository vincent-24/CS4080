#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct DoublyLinkedList {
    struct Node* head;
    struct Node* tail;
};

void initList(struct DoublyLinkedList* list) {
    list->head = NULL;
    list->tail = NULL;
}

struct Node {
    char* data;
    struct Node* next;
    struct Node* prev;
};

struct Node* createNode(char* string) {
    struct Node* node = malloc(sizeof(struct Node));
    node->data = malloc(strlen(string) + 1);
    strcpy(node->data, string);
    node->next = NULL;
    node->prev = NULL;

    return node; 
}

void insert(struct DoublyLinkedList* list, char* string) {
    struct Node* newNode = createNode(string);

    if (list->head == NULL) {
        list->head = newNode;
        list->tail = newNode;
    } else {
        newNode->next = list->head;
        list->head->prev = newNode;
        list->head = newNode;
    }
}

struct Node* find(struct DoublyLinkedList* list, char* target) {
    struct Node* current = list->head;

    while (current != NULL) {
        if (strcmp(current->data, target) == 0) {
            return current;
        }
        current = current->next;
    }

    return NULL;
}

void delete(struct DoublyLinkedList* list, struct Node* nodeToDelete) {
    if (nodeToDelete == NULL) {
        return;
    }

    if (nodeToDelete->prev != NULL) {
        nodeToDelete->prev->next = nodeToDelete->next;
    } else {
        list->head = nodeToDelete->next;
    }

    if (nodeToDelete->next != NULL) {
        nodeToDelete->next->prev = nodeToDelete->prev;
    } else {
        list->tail = nodeToDelete->prev;
    }

    free(nodeToDelete->data);
    free(nodeToDelete);
}

int main(void) {
    printf("Hello, World!\n\n");

    struct DoublyLinkedList list;
    initList(&list);

    insert(&list, "First");
    insert(&list, "Second");
    insert(&list, "Third");

    printf("List after insertions:\n");
    struct Node* current = list.head;
    while (current != NULL) {
        printf("%s\n", current->data);
        current = current->next;
    }  
    printf("\n");

    struct Node* foundNode = find(&list, "Second");
    if (foundNode != NULL) {
        printf("Found node with data: %s\n", foundNode->data);
    } else {
        printf("Node not found.\n");
    }

    delete(&list, foundNode);
    printf("List after deletion:\n");
    current = list.head;
    while (current != NULL) {
        printf("%s\n", current->data);
        current = current->next;
    }  
    printf("\n");

    return 0;
}