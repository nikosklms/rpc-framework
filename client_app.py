import threading
import client_api  # Το API που χειρίζεται τις UDP επικοινωνίες

NUM_CLIENTS = 5  # Αριθμός client threads

current_client = 0
lock = threading.Lock()
cond = threading.Condition(lock)

def client_task(client_id, p):
    """Κάθε client στέλνει το request με το δικό του αριθμό p και περιμένει απάντηση πριν επιτρέψει στον επόμενο να συνεχίσει."""
    global current_client
    # Αναμονή μέχρι να φτάσει η σειρά του client
    with cond:
        while client_id != current_client:
            cond.wait()

    # Δημιουργία request με το p που θα ελεγχθεί για primality
    reqbuf = f"{p}".encode()
    reqlen = len(reqbuf)
    
    # Δημιουργούμε ένα buffer για την απάντηση (π.χ. 1024 bytes) και παίρνουμε το μήκος του
    rspbuf = bytearray(1024)
    rsplen = [0]
    
    # Κλήση της συνάρτησης doRequestReply με όλα τα απαιτούμενα ορίσματα
    status = client_api.doRequestReply(1, reqbuf, reqlen, rspbuf, rsplen)
    
    # Αποκωδικοποίηση της απάντησης (μπορείτε να προσαρμόσετε την επεξεργασία ανάλογα με το τι περιέχει)
    response_str = rspbuf.decode()
    print(f"[APP] Client {client_id} με p={p} έλαβε απάντηση: len={rsplen}, data={rspbuf.decode('utf-8', errors='ignore')}")

    
    # Επιτρέπουμε στον επόμενο client να συνεχίσει
    with cond:
        current_client += 1
        cond.notify_all()

if __name__ == "__main__":
    # Αρχικοποίηση του client API με το config αρχείο
    config_filename = "config.txt"
    client_api.init(config_filename)

    # Ζητάμε από τον χρήστη να εισάγει τις τιμές p για κάθε client
    p_values = [17, 23, 19, 20, 11]

    # Δημιουργία και εκκίνηση των client threads
    threads = []
    for i in range(NUM_CLIENTS):
        t = threading.Thread(target=client_task, args=(i, p_values[i]))
        threads.append(t)
        t.start()

    # Αναμονή για την ολοκλήρωση όλων των threads
    for t in threads:
        t.join()